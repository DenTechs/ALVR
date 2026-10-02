#pragma once

#ifdef ALVR_PYROWAVE

#include "shared/d3drender.h"

#include <d3d11_4.h>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>
#include <wrl.h>

#include "pyrowave.h"

class PyrowaveFoveationEncoder {
public:
    explicit PyrowaveFoveationEncoder(std::shared_ptr<CD3DRender> d3dRender);
    ~PyrowaveFoveationEncoder();

    void Initialize();
    void Encode(ID3D11Texture2D* source, uint64_t targetTimestampNs);

private:
    void Shutdown();

    std::shared_ptr<CD3DRender> m_d3dRender;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_atlas;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_device5;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    pyrowave_device m_device = nullptr;
    pyrowave_image m_image = nullptr;
    pyrowave_image_view m_imageView = {};
    pyrowave_encoder m_encoder = nullptr;
    std::vector<uint8_t> m_bitstream;
    uint32_t m_sourceWidth = 0;
    uint32_t m_sourceHeight = 0;
    uint32_t m_eyeWidth = 0;
    uint32_t m_cropWidth = 0;
    uint32_t m_cropHeight = 0;
    uint32_t m_chromaSubsampling = 1;
    float m_bitsPerPixel = 1.0f;
    float m_edgeBlend = 0.08f;
};

#endif
