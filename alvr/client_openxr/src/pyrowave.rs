use alvr_common::glam::UVec2;
use alvr_packets::PyrowaveFoveationPacketHeader;
use alvr_session::{PyrowaveChromaSubsampling, PyrowaveFoveationConfig, pyrowave_crop_resolution};
use std::{
    collections::VecDeque,
    ffi::c_void,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TryRecvError},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

#[repr(C)]
struct DecoderCreateInfo {
    device: *mut c_void,
    width: i32,
    height: i32,
    chroma: i32,
    fragment_path: bool,
}

#[repr(C)]
struct CpuBuffer {
    data: [*mut c_void; 3],
    row_stride_in_bytes: [usize; 3],
    plane_size_in_bytes: [usize; 3],
    width: i32,
    height: i32,
    format: i32,
}

#[repr(C)]
struct DecoderOpaque {
    _private: [u8; 0],
}

#[link(name = "pyrowave-shared")]
unsafe extern "C" {
    fn pyrowave_get_api_version(major: *mut u32, minor: *mut u32, patch: *mut u32);
    fn pyrowave_create_default_device(device: *mut *mut c_void) -> i32;
    fn pyrowave_device_destroy(device: *mut c_void);
    fn pyrowave_decoder_create(
        info: *const DecoderCreateInfo,
        decoder: *mut *mut DecoderOpaque,
    ) -> i32;
    fn pyrowave_decoder_device_prefers_fragment_path(device: *mut c_void) -> bool;
    fn pyrowave_decoder_destroy(decoder: *mut DecoderOpaque);
    fn pyrowave_decoder_push_packet(
        decoder: *mut DecoderOpaque,
        data: *const c_void,
        size: usize,
    ) -> i32;
    fn pyrowave_decoder_decode_is_ready(decoder: *mut DecoderOpaque, allow_partial: bool) -> bool;
    fn pyrowave_decoder_decode_cpu_buffer_synchronous(
        decoder: *mut DecoderOpaque,
        buffer: *const CpuBuffer,
    ) -> i32;
}

struct InputPacket {
    header: PyrowaveFoveationPacketHeader,
    payload: Vec<u8>,
}

pub struct DecodedFocusFrame {
    pub header: PyrowaveFoveationPacketHeader,
    pub rgba: Vec<u8>,
}

pub struct PyrowaveFoveationDecoder {
    input: Option<SyncSender<InputPacket>>,
    output: Receiver<DecodedFocusFrame>,
    pending: VecDeque<DecodedFocusFrame>,
    worker: Option<JoinHandle<()>>,
    running: Arc<AtomicBool>,
    crop_resolution: UVec2,
}

impl PyrowaveFoveationDecoder {
    pub fn new(
        view_resolution: UVec2,
        config: &PyrowaveFoveationConfig,
    ) -> alvr_common::anyhow::Result<Self> {
        let crop_resolution = pyrowave_crop_resolution(view_resolution, config.region_size);
        let (input, input_rx) = mpsc::sync_channel::<InputPacket>(2);
        let (output_tx, output) = mpsc::sync_channel::<DecodedFocusFrame>(2);
        let (startup_tx, startup_rx) = mpsc::sync_channel::<Result<(), String>>(1);
        let running = Arc::new(AtomicBool::new(true));
        let worker_running = Arc::clone(&running);
        let chroma = match config.chroma_subsampling {
            PyrowaveChromaSubsampling::Yuv420 => 0,
            PyrowaveChromaSubsampling::Yuv444 => 1,
        };

        let worker = thread::Builder::new()
            .name("PyroWave focus decoder".into())
            .spawn(move || unsafe {
                let mut major = 0;
                let mut minor = 0;
                let mut patch = 0;
                pyrowave_get_api_version(&mut major, &mut minor, &mut patch);
                if major != 0 || minor != 6 {
                    let message = format!(
                        "PyroWave focus decoder API mismatch: expected 0.6.x, found {major}.{minor}.{patch}"
                    );
                    alvr_common::error!("{message}");
                    let _ = startup_tx.send(Err(message));
                    return;
                }

                let mut device = std::ptr::null_mut();
                if pyrowave_create_default_device(&mut device) != 0 || device.is_null() {
                    alvr_common::error!("Could not create PyroWave Vulkan decoder device");
                    let _ = startup_tx.send(Err(
                        "Could not create PyroWave Vulkan decoder device".to_owned(),
                    ));
                    return;
                }
                let dimensions = UVec2::new(crop_resolution.x * 2, crop_resolution.y);
                let info = DecoderCreateInfo {
                    device,
                    width: dimensions.x as i32,
                    height: dimensions.y as i32,
                    chroma,
                    fragment_path: pyrowave_decoder_device_prefers_fragment_path(device),
                };
                let mut decoder = std::ptr::null_mut();
                if pyrowave_decoder_create(&info, &mut decoder) != 0 || decoder.is_null() {
                    alvr_common::error!("Could not create PyroWave focus decoder");
                    pyrowave_device_destroy(device);
                    let _ = startup_tx.send(Err("Could not create PyroWave focus decoder".to_owned()));
                    return;
                }
                let _ = startup_tx.send(Ok(()));

                let width = dimensions.x as usize;
                let height = dimensions.y as usize;
                let is_420 = chroma == 0;
                let chroma_step = if is_420 { 2 } else { 1 };
                let chroma_width = if is_420 { width / 2 } else { width };
                let chroma_height = if is_420 { height / 2 } else { height };
                let y_size = width * height;
                let chroma_size = chroma_width * chroma_height;
                let mut y = vec![0u8; y_size];
                let mut cb = vec![0u8; chroma_size];
                let mut cr = vec![0u8; chroma_size];
                let mut rgba = vec![0u8; width * height * 4];

                loop {
                    let packet = match input_rx.recv_timeout(Duration::from_millis(100)) {
                        Ok(packet) => packet,
                        Err(RecvTimeoutError::Timeout)
                            if worker_running.load(Ordering::Relaxed) =>
                        {
                            continue;
                        }
                        Err(_) => break,
                    };
                    if pyrowave_decoder_push_packet(
                        decoder,
                        packet.payload.as_ptr().cast(),
                        packet.payload.len(),
                    ) != 0
                        || !pyrowave_decoder_decode_is_ready(decoder, false)
                    {
                        continue;
                    }

                    let cpu_format = if is_420 { 1 } else { 2 }; // YUV420P / YUV444P
                    let cpu_buffer = CpuBuffer {
                        data: [
                            y.as_mut_ptr().cast(),
                            cb.as_mut_ptr().cast(),
                            cr.as_mut_ptr().cast(),
                        ],
                        row_stride_in_bytes: [width, chroma_width, chroma_width],
                        plane_size_in_bytes: [y_size, chroma_size, chroma_size],
                        width: width as i32,
                        height: height as i32,
                        format: cpu_format,
                    };
                    if pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpu_buffer) != 0 {
                        continue;
                    }

                    for row in 0..height {
                        for column in 0..width {
                            let chroma_index = (row / chroma_step) * chroma_width
                                + column / chroma_step;
                            let luma = y[row * width + column] as f32 / 255.0;
                            let u = cb[chroma_index] as f32 / 255.0 - 128.0 / 255.0;
                            let v = cr[chroma_index] as f32 / 255.0 - 128.0 / 255.0;
                            let offset = (row * width + column) * 4;
                            rgba[offset] = ((luma + 1.5748 * v).clamp(0.0, 1.0) * 255.0) as u8;
                            rgba[offset + 1] =
                                ((luma - 0.1873 * u - 0.4681 * v).clamp(0.0, 1.0) * 255.0) as u8;
                            rgba[offset + 2] = ((luma + 1.8556 * u).clamp(0.0, 1.0) * 255.0) as u8;
                            rgba[offset + 3] = 255;
                        }
                    }

                    let frame = DecodedFocusFrame {
                        header: packet.header,
                        rgba: std::mem::replace(&mut rgba, vec![0u8; width * height * 4]),
                    };
                    // The bounded render queue drops the newest frame if it is already full;
                    // render-side timestamp matching discards stale frames.
                    let _ = output_tx.try_send(frame);
                }

                pyrowave_decoder_destroy(decoder);
                pyrowave_device_destroy(device);
            })?;

        match startup_rx.recv() {
            Ok(Ok(())) => {}
            Ok(Err(message)) => {
                let _ = worker.join();
                return Err(alvr_common::anyhow::anyhow!(message));
            }
            Err(error) => {
                let _ = worker.join();
                return Err(alvr_common::anyhow::anyhow!(
                    "PyroWave focus decoder worker exited during startup: {error}"
                ));
            }
        }

        Ok(Self {
            input: Some(input),
            output,
            pending: VecDeque::new(),
            worker: Some(worker),
            running,
            crop_resolution,
        })
    }

    pub fn input_callback(
        &self,
    ) -> impl FnMut(&PyrowaveFoveationPacketHeader, &[u8]) -> bool + Send + 'static {
        let input = self.input.as_ref().unwrap().clone();
        let crop_resolution = self.crop_resolution;
        move |header, payload| {
            let valid_rect = |rect: &[f32; 4]| {
                rect.iter().all(|value| value.is_finite())
                    && rect[0] >= 0.0
                    && rect[1] >= 0.0
                    && rect[2] > 0.0
                    && rect[3] > 0.0
                    && rect[0] + rect[2] <= 1.001
                    && rect[1] + rect[3] <= 1.001
            };
            if header.crop_resolution != crop_resolution
                || !header.edge_blend.is_finite()
                || !(0.0..=0.25).contains(&header.edge_blend)
                || !header.source_rects.iter().all(valid_rect)
                || payload.is_empty()
            {
                return false;
            }
            input
                .try_send(InputPacket {
                    header: header.clone(),
                    payload: payload.to_vec(),
                })
                .is_ok()
        }
    }

    pub fn take_for_timestamp(&mut self, timestamp: Duration) -> Option<DecodedFocusFrame> {
        loop {
            match self.output.try_recv() {
                Ok(frame) => {
                    if self.pending.len() == 2 {
                        self.pending.pop_front();
                    }
                    self.pending.push_back(frame);
                }
                Err(TryRecvError::Empty | TryRecvError::Disconnected) => break,
            }
        }

        while self
            .pending
            .front()
            .is_some_and(|frame| frame.header.timestamp < timestamp)
        {
            self.pending.pop_front();
        }
        let matching = self
            .pending
            .iter()
            .position(|frame| frame.header.timestamp == timestamp)?;
        self.pending.remove(matching)
    }
}

impl Drop for PyrowaveFoveationDecoder {
    fn drop(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        self.input.take();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
