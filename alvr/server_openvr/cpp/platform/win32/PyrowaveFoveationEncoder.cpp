#ifdef ALVR_PYROWAVE

#include "PyrowaveFoveationEncoder.h"

#include "alvr_server/Settings.h"
#include "alvr_server/Logger.h"
#include "alvr_server/Utils.h"
#include "alvr_server/bindings.h"

#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <limits>

static_assert(
    PYROWAVE_API_VERSION_MAJOR == 0 && PYROWAVE_API_VERSION_MINOR == 6,
    "ALVR's Android decoder FFI currently requires PyroWave 0.6.x"
);

using Microsoft::WRL::ComPtr;

static uint32_t EvenCropDimension(float fraction, uint32_t extent) {
    const uint32_t maxEven = extent & ~1u;
    if (maxEven == 0)
        return std::max(2u, extent);
    const float scaled = std::clamp(fraction * extent, 2.0f, (float)maxEven);
    return (uint32_t)std::round(scaled) & ~1u;
}

PyrowaveFoveationEncoder::PyrowaveFoveationEncoder(std::shared_ptr<CD3DRender> d3dRender)
    : m_d3dRender(std::move(d3dRender)) {
    m_sourceWidth = Settings_Instance()->m_renderWidth;
    m_sourceHeight = Settings_Instance()->m_renderHeight;
    m_eyeWidth = m_sourceWidth / 2;
    m_cropWidth = EvenCropDimension(Settings_Instance()->m_pyrowaveRegionSize[0], m_eyeWidth);
    m_cropHeight = EvenCropDimension(Settings_Instance()->m_pyrowaveRegionSize[1], m_sourceHeight);
    m_chromaSubsampling = Settings_Instance()->m_pyrowaveChromaSubsampling;
    m_bitsPerPixel = Settings_Instance()->m_pyrowaveBitsPerPixel;
    m_edgeBlend = Settings_Instance()->m_pyrowaveEdgeBlend;
}

PyrowaveFoveationEncoder::~PyrowaveFoveationEncoder() { Shutdown(); }

void PyrowaveFoveationEncoder::Initialize() {
    uint32_t major = 0, minor = 0, patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);
    if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
        throw MakeException(
            "PyroWave API mismatch: headers %u.%u.%u, library %u.%u.%u",
            PYROWAVE_API_VERSION_MAJOR,
            PYROWAVE_API_VERSION_MINOR,
            PYROWAVE_API_VERSION_PATCH,
            major,
            minor,
            patch
        );
    }

    if (m_sourceWidth < 4 || m_sourceHeight < 2 || m_cropWidth > m_eyeWidth
        || m_cropHeight > m_sourceHeight || m_chromaSubsampling > 1) {
        throw MakeException("Invalid PyroWave foveation dimensions or chroma mode.");
    }

    HRESULT hr = m_d3dRender->GetDevice()->QueryInterface(IID_PPV_ARGS(&m_device5));
    if (FAILED(hr))
        throw MakeException("PyroWave requires ID3D11Device5 for GPU ordering.");
    hr = m_d3dRender->GetContext()->QueryInterface(IID_PPV_ARGS(&m_context4));
    if (FAILED(hr))
        throw MakeException("PyroWave requires ID3D11DeviceContext4.");

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_d3dRender->GetDevice()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr))
        throw MakeException("PyroWave could not query IDXGIDevice.");
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr))
        throw MakeException("PyroWave could not find the D3D adapter.");
    DXGI_ADAPTER_DESC adapterDesc = {};
    hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr))
        throw MakeException("PyroWave could not read the D3D adapter LUID.");

    static_assert(sizeof(LUID) == sizeof(pyrowave_luid), "PyroWave LUID size mismatch");
    pyrowave_result result = pyrowave_create_device_by_compat(
        0,
        0,
        nullptr,
        nullptr,
        reinterpret_cast<const pyrowave_luid*>(&adapterDesc.AdapterLuid),
        &m_device
    );
    if (result != PYROWAVE_SUCCESS || !m_device)
        throw MakeException("PyroWave Vulkan device creation failed: %d", (int)result);
    if (!pyrowave_device_confirm_interop_support(m_device))
        throw MakeException("PyroWave GPU does not support external image interop.");

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_cropWidth * 2;
    desc.Height = m_cropHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Match PyroWave's D3D11 interop sample: share through an NT handle and
    // use the explicit D3D11 fence before Vulkan reads the copied atlas.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    hr = m_d3dRender->GetDevice()->CreateTexture2D(&desc, nullptr, &m_atlas);
    if (FAILED(hr))
        throw MakeException("Could not create shared PyroWave focus atlas.");

    ComPtr<IDXGIResource1> resource;
    hr = m_atlas.As(&resource);
    if (FAILED(hr))
        throw MakeException("PyroWave focus atlas is not a shareable DXGI resource.");
    HANDLE sharedHandle = nullptr;
    hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle);
    if (FAILED(hr))
        throw MakeException("Could not create PyroWave focus atlas handle.");

    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { desc.Width, desc.Height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    pyrowave_image_create_info importInfo = {};
    importInfo.device = m_device;
    importInfo.external_handle = (pyrowave_os_handle)sharedHandle;
    importInfo.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    importInfo.image_create_info = &imageInfo;
    result = pyrowave_image_create(&importInfo, &m_image);
    if (result != PYROWAVE_SUCCESS) {
        CloseHandle(sharedHandle);
        throw MakeException("PyroWave focus atlas import failed: %d", (int)result);
    }

    result = pyrowave_image_get_image_view(
        m_image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT,
        &m_imageView
    );
    if (result != PYROWAVE_SUCCESS)
        throw MakeException("PyroWave focus atlas image view creation failed: %d", (int)result);

    pyrowave_encoder_create_info encoderInfo = {};
    encoderInfo.device = m_device;
    encoderInfo.width = (int)m_cropWidth * 2;
    encoderInfo.height = (int)m_cropHeight;
    encoderInfo.chroma = m_chromaSubsampling == 0 ? PYROWAVE_CHROMA_SUBSAMPLING_420
                                                 : PYROWAVE_CHROMA_SUBSAMPLING_444;
    result = pyrowave_encoder_create(&encoderInfo, &m_encoder);
    if (result != PYROWAVE_SUCCESS)
        throw MakeException("PyroWave focus encoder creation failed: %d", (int)result);

    hr = m_device5->CreateFence(
        0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)
    );
    if (FAILED(hr))
        throw MakeException("Could not create PyroWave D3D11 fence.");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw MakeException("Could not create PyroWave D3D11 fence event.");

    const size_t pixelCount = (size_t)m_cropWidth * m_cropHeight * 2;
    m_bitstream.resize(pixelCount * 3 + (1u << 20));
}

void PyrowaveFoveationEncoder::Encode(ID3D11Texture2D* source, uint64_t targetTimestampNs) {
    if (!source || !m_encoder)
        return;

    FfiPyrowaveCropRects cropData = GetEyeTrackedPyrowaveCropRects(targetTimestampNs);
    if (!cropData.valid)
        return;

    ID3D11DeviceContext* context = m_d3dRender->GetContext();
    float rects[2][4] = {};
    for (uint32_t eye = 0; eye < 2; eye++) {
        const float* gazeRect = cropData.rects[eye];
        const float maxX = (float)(m_eyeWidth - m_cropWidth) / m_eyeWidth;
        const float maxY = (float)(m_sourceHeight - m_cropHeight) / m_sourceHeight;
        const float x = std::clamp(std::isfinite(gazeRect[0]) ? gazeRect[0] : 0.5f, 0.0f, maxX);
        const float y = std::clamp(std::isfinite(gazeRect[1]) ? gazeRect[1] : 0.5f, 0.0f, maxY);
        const uint32_t left = (uint32_t)std::round(x * m_eyeWidth);
        const uint32_t top = (uint32_t)std::round(y * m_sourceHeight);
        const uint32_t clampedLeft = std::min(left, m_eyeWidth - m_cropWidth);
        const uint32_t clampedTop = std::min(top, m_sourceHeight - m_cropHeight);

        D3D11_BOX sourceBox = {};
        sourceBox.left = eye * m_eyeWidth + clampedLeft;
        sourceBox.top = clampedTop;
        sourceBox.front = 0;
        sourceBox.right = sourceBox.left + m_cropWidth;
        sourceBox.bottom = sourceBox.top + m_cropHeight;
        sourceBox.back = 1;
        context->CopySubresourceRegion(
            m_atlas.Get(), 0, eye * m_cropWidth, 0, 0, source, 0, &sourceBox
        );

        rects[eye][0] = (float)clampedLeft / m_eyeWidth;
        rects[eye][1] = (float)clampedTop / m_sourceHeight;
        rects[eye][2] = (float)m_cropWidth / m_eyeWidth;
        rects[eye][3] = (float)m_cropHeight / m_sourceHeight;
    }

    if (FAILED(m_context4->Signal(m_fence.Get(), ++m_fenceValue)))
        return;
    context->Flush();
    if (FAILED(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent)))
        return;
    if (WaitForSingleObject(m_fenceEvent, 250) != WAIT_OBJECT_0) {
        Error("Timed out waiting for the PyroWave source texture fence.\n");
        return;
    }

    pyrowave_gpu_external_reference ref = { m_image, VK_QUEUE_FAMILY_EXTERNAL };
    pyrowave_gpu_sync_operation sync = {};
    sync.images = &ref;
    sync.num_images = 1;
    pyrowave_scaled_encode_info scaling = {};
    scaling.view = m_imageView;
    scaling.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaling.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaling.intermediate_plane_format = VK_FORMAT_R8_UNORM;
    scaling.ycbcr_chroma_midpoint = 128.0f / 255.0f;
    scaling.force_linear_filtering = false;
    scaling.skip_dither = false;

    pyrowave_rate_control rateControl = {};
    rateControl.maximum_bitstream_size = (size_t)std::ceil(
        (2.0 * m_cropWidth * m_cropHeight * std::clamp(m_bitsPerPixel, 0.1f, 4.0f)) / 8.0
    );
    pyrowave_result result = pyrowave_encoder_encode_gpu_scaled_synchronous(
        m_encoder, &sync, &sync, &scaling, &rateControl
    );
    if (result != PYROWAVE_SUCCESS)
        return;

    size_t packetCount = 0;
    if (pyrowave_encoder_compute_num_packets(m_encoder, m_bitstream.size(), &packetCount)
            != PYROWAVE_SUCCESS
        || packetCount != 1)
        return;
    pyrowave_packet packet = {};
    size_t outputCount = 0;
    if (pyrowave_encoder_packetize(
            m_encoder,
            &packet,
            m_bitstream.size(),
            &outputCount,
            m_bitstream.data(),
            m_bitstream.size()
        )
            != PYROWAVE_SUCCESS
        || outputCount != 1
        || packet.size > (size_t)std::numeric_limits<int>::max())
        return;

    PyrowaveFoveationSend(
        targetTimestampNs,
        (const float(*)[4])rects,
        m_cropWidth,
        m_cropHeight,
        m_edgeBlend,
        m_bitstream.data() + packet.offset,
        (int)packet.size
    );
}

void PyrowaveFoveationEncoder::Shutdown() {
    if (m_encoder) {
        pyrowave_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    if (m_image) {
        pyrowave_image_destroy(m_image);
        m_image = nullptr;
    }
    if (m_device) {
        pyrowave_device_destroy(m_device);
        m_device = nullptr;
    }
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

#endif
