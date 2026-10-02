fn main() {
    println!("cargo:rustc-check-cfg=cfg(alvr_pyrowave_foveation)");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("android")
        && std::env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("aarch64")
    {
        let pyrowave_lib_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../deps/android_openxr/arm64-v8a");
        let pyrowave_lib = pyrowave_lib_dir.join("libpyrowave-shared.so");
        println!("cargo:rerun-if-changed={}", pyrowave_lib.display());
        if pyrowave_lib.exists() {
            println!("cargo:rustc-cfg=alvr_pyrowave_foveation");
            println!(
                "cargo:rustc-link-search=native={}",
                pyrowave_lib_dir.display()
            );
            println!("cargo:rustc-link-lib=pyrowave-shared");
        }
    }
}
