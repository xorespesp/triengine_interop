#include <triengine_interop/surface/detail/shared_surface_consumer.hh>
#include "unique_handle.hh"
#include <triengine_interop/proto/ipc_proto.hh>

#include <triengine_interop/utility/logger.hh>

#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <memory>
#include <system_error>
#include <chrono>

namespace triengine_interop::surface::detail
{
    namespace proto = triengine_interop::proto;
    using Microsoft::WRL::ComPtr;

    namespace
    {
        // Create a D3D11 device/context on the adapter that owns the renderer's surface.
        bool create_device(
            const LUID target_adapter_luid,
            ComPtr<ID3D11Device2>& out_device,
            ComPtr<ID3D11DeviceContext2>& out_context)
        {
            // NOTE: 반드시 CreateDXGIFactory2 함수를 사용해서 DXGI 1.2 버전 이상의 DXGI 팩토리(`IDXGIFactory`)를 생성해줘야 함.
            // (`ID3D11Device::CreateTexture2D: D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX is only available for devices created off of Dxgi1.1 factories or later.` D3D11 오류 방지)
            ComPtr<IDXGIFactory2> dxgi_factory2;
            if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi_factory2)))) {
                TEIO_ERROR("Failed to create DXGI factory");
                return false;
            }

            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIAdapter> enum_adapter;
            for (UINT i = 0; dxgi_factory2->EnumAdapters(i, &enum_adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC desc{};
                enum_adapter->GetDesc(&desc);
                if (0 == std::memcmp(&desc.AdapterLuid, &target_adapter_luid, sizeof(LUID))) {
                    adapter = enum_adapter;
                    break;
                }
            }
            if (!adapter) {
                TEIO_ERROR("Matching adapter not found");
                return false;
            }

            UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
            device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            ComPtr<ID3D11Device> dx11_device0;
            ComPtr<ID3D11DeviceContext> dx11_device_context0;
            if (FAILED(::D3D11CreateDevice(
                adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                device_flags,
                nullptr, 0,
                D3D11_SDK_VERSION,
                &dx11_device0, nullptr, &dx11_device_context0)))
            {
                TEIO_ERROR("Failed to create D3D11 device");
                return false;
            }

            // Convert `ID3D11Device` -> `ID3D11Device2` (Higher version object)
            if (FAILED(dx11_device0.As(&out_device))) {
                TEIO_ERROR("Failed to convert to ID3D11Device2");
                return false;
            }

            // Convert `ID3D11DeviceContext` -> `ID3D11DeviceContext2` (Higher version object)
            if (FAILED(dx11_device_context0.As(&out_context))) {
                TEIO_ERROR("Failed to convert to ID3D11DeviceContext2");
                return false;
            }
            return true;
        }

        // Compile an HLSL shader source into a bytecode blob. Returns nullptr on failure.
        ComPtr<ID3DBlob> compile_shader(
            const std::string_view source,
            const char* const entry_point,
            const char* const target,
            const D3D_SHADER_MACRO* const defines = nullptr)
        {
            ComPtr<ID3DBlob> blob, err_blob;
            const HRESULT hr = ::D3DCompile(
                source.data(), source.size(),
                nullptr, defines, nullptr,
                entry_point, target,
                0, 0,
                &blob, &err_blob
            );
            if (FAILED(hr)) {
                TEIO_ERROR("failed to compile {} shader (HRESULT: {:08X}): {}"
                    , target
                    , static_cast<uint32_t>(hr)
                    , err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : ""
                );
                return nullptr;
            }
            return blob;
        }

        // Compile the full-screen blit shaders and create the sampler.
        bool build_blit_pipeline(
            ID3D11Device2* const device,
            const surface_render_options& config,
            ComPtr<ID3D11VertexShader>& out_vs,
            ComPtr<ID3D11PixelShader>& out_ps,
            ComPtr<ID3D11SamplerState>& out_sampler)
        {
            // Base full-screen quad rendering shader template with preprocessor conditionals
            static const std::string kBlitShaderTemplate = R"hlsl(
                Texture2D g_texture : register(t0);
                SamplerState g_sampler : register(s0);

                struct VS_OUT {
                    float4 pos : SV_POSITION;
                    float2 uv : TEXCOORD;
                };

                VS_OUT VS(uint id : SV_VertexID) {
                    VS_OUT output;
                    // Full-screen triangle UVs: (0,0), (2,0), (0,2)
                    output.uv = float2((id << 1) & 2, id & 2);
                    // Full-screen triangle positions: (-1,1), (3,1), (-1,-3)
                    output.pos = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
                    // Flip Y for correct rendering
                    output.pos.y = -output.pos.y;
                    return output;
                }

                float4 PS(VS_OUT input) : SV_TARGET {
                    float2 uv = input.uv;

                #ifdef FLIP_Y_AXIS
                    // Flip Y-axis (if needed)
                    uv.y = 1.0 - uv.y;
                #endif

                    float4 color = g_texture.Sample(g_sampler, uv);

                #ifdef CONVERT_RGBA_TO_BGRA
                    color = float4(color.b, color.g, color.r, color.a); // Swap R and B channels
                #endif

                    return color;
                }
            )hlsl";

            // Compile vertex shader
            ComPtr<ID3DBlob> vs_blob = compile_shader(kBlitShaderTemplate, "VS", "vs_5_0");
            if (!vs_blob) {
                return false;
            }
            if (FAILED(device->CreateVertexShader(
                vs_blob->GetBufferPointer(),
                vs_blob->GetBufferSize(),
                nullptr,
                &out_vs)))
            {
                TEIO_ERROR("Failed to create vertex shader");
                return false;
            }

            // Compile pixel shader
            std::vector<D3D_SHADER_MACRO> defines;

            // Need Y-flip because OpenGL uses bottom-left origin while DirectX uses top-left
            if (config.flip_y) {
                defines.push_back(D3D_SHADER_MACRO{ "FLIP_Y_AXIS", "1" });
            }

            // NOTE: No manual color-conversion needed when rendering RGBA texture (OpenGL) to BGRA render target (Flutter),
            // GPU handles the conversion automatically.
            // when you load the texture, it gets 'swizzled' if needed to the standard Red, Green, and Blue channels
            // and when you write to the render target the same thing happens depending on the format.
            // so manual pixel shader conversion would cause double-swapping and corrupt colors.
            // Ref: https://stackoverflow.com/a/46369577/3865427
            if (config.convert_rgba_to_bgra) {
                defines.push_back(D3D_SHADER_MACRO{ "CONVERT_RGBA_TO_BGRA", "1" });
            }

            // Add a null terminator to the defines array
            if (!defines.empty()) {
                defines.push_back(D3D_SHADER_MACRO{ nullptr, nullptr });
            }

            ComPtr<ID3DBlob> ps_blob = compile_shader(
                kBlitShaderTemplate, "PS", "ps_5_0",
                defines.empty() ? nullptr : defines.data()
            );
            if (!ps_blob) {
                return false;
            }
            if (FAILED(device->CreatePixelShader(
                ps_blob->GetBufferPointer(),
                ps_blob->GetBufferSize(),
                nullptr,
                &out_ps)))
            {
                TEIO_ERROR("Failed to create pixel shader");
                return false;
            }

            // Create Sampler State
            D3D11_SAMPLER_DESC sampDesc{};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sampDesc.MinLOD = 0;
            sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sampDesc, &out_sampler))) {
                TEIO_ERROR("Failed to create sampler state");
                return false;
            }
            return true;
        }

        // Open a shared keyed-mutex texture from a renderer-owned NT handle.
        // Uses OpenSharedResource1, which requires the handle to be duplicated into
        // the current process first.
        ComPtr<ID3D11Texture2D> open_shared_texture(
            ID3D11Device2* const device,
            const HANDLE shared_handle,
            const HANDLE owner_process)
        {
            ComPtr<ID3D11Texture2D> texture;

            // OpenSharedResource1(혹은 OpenSharedResourceByName)를 사용하여 client측에서 생성한 NT 핸들 획득 & 공유 텍스처 생성
            // (OpenSharedResource1 함수를 사용하는 경우, DuplicateHandle을 사용하여 전달받은 공유 텍스처 핸들을 현재 프로세스에서 유효한 핸들로 복제해야 함)
            HANDLE duplicated_handle{};
            if (!::DuplicateHandle(
                owner_process, // 원본 핸들을 소유하고 있는 소스 프로세스 핸들
                shared_handle, // IPC로 수신한 원본 핸들 값
                ::GetCurrentProcess(), // 핸들을 복제해 올 타겟 프로세스 핸들 (현재 프로세스)
                &duplicated_handle, // 복제된 핸들을 저장할 포인터
                0, // 접근 권한 (0은 원본과 동일한 권한임을 의미)
                FALSE, // 핸들 상속 여부
                DUPLICATE_SAME_ACCESS // 원본과 동일한 접근 권한으로 복제
            )) {
                TEIO_ERROR("Failed to duplicate shared texture handle. (error: {})", ::GetLastError());
                return nullptr;
            }

            unique_handle duplicated_handle_guard{ duplicated_handle }; // 핸들의 자동 해제를 위한 RAII 핸들 래퍼
            if (HRESULT hr = device->OpenSharedResource1(
                duplicated_handle_guard.get(),
                IID_PPV_ARGS(&texture));
                FAILED(hr))
            {
                TEIO_ERROR("Failed to open shared texture resource. (HRESULT: {:08X})", static_cast<uint32_t>(hr));
                return nullptr;
            }

            return texture;
        }

    } // namespace

    class shared_surface_consumer::impl
    {
    public:
        impl() = default;
        ~impl() { this->destroy(); }

        ID3D11Device2* get_dx11_device() const noexcept { return _device.Get(); }
        ID3D11DeviceContext2* get_dx11_context() const noexcept { return _context.Get(); }
        int32_t get_width() const noexcept { return _width; }
        int32_t get_height() const noexcept { return _height; }

        bool is_created() const noexcept { return _created; }

        bool create(
            ipc_client& cli, 
            const int32_t initial_width, 
            const int32_t initial_height, 
            const surface_render_options& config)
        {
            if (_created) {
                TEIO_ERROR("consumer already created");
                return false;
            }

            // 1. init handshake
            proto::packets::init_response_t init_rep{};
            if (!this->_request_init(cli, initial_width, initial_height, init_rep)) {
                return false;
            }

            // 2. open the renderer process (needed to duplicate the shared handle)
            unique_handle renderer_process_handle{ ::OpenProcess(
                PROCESS_DUP_HANDLE | SYNCHRONIZE, 
                FALSE, 
                init_rep.renderer_process_id
            ) };
            if (!renderer_process_handle) {
                TEIO_ERROR("failed to open renderer process (pid: {})", init_rep.renderer_process_id);
                return false;
            }

            // 3. create the D3D11 device on the renderer's adapter
            ComPtr<ID3D11Device2> new_device;
            ComPtr<ID3D11DeviceContext2> new_context;
            if (!create_device(
                init_rep.target_adapter_luid, 
                new_device, 
                new_context)) {
                return false;
            }

            // 4. build the shared-surface resources (shared tex + keyed mutex + copy + SRV)
            auto new_surface_resources = surface_resources::build(
                new_device.Get(), 
                renderer_process_handle.get(), 
                init_rep.surface_handle
            );
            if (!new_surface_resources) {
                return false;
            }

            // 5. build the blit pipeline (VS / PS / sampler)
            ComPtr<ID3D11VertexShader> new_vs;
            ComPtr<ID3D11PixelShader> new_ps;
            ComPtr<ID3D11SamplerState> new_sampler;
            if (!build_blit_pipeline(
                new_device.Get(), 
                config, 
                new_vs, new_ps, 
                new_sampler)) {
                return false;
            }

            // 6. commit
            _config = config;
            _renderer_process_handle = std::move(renderer_process_handle);
            _device = std::move(new_device);
            _context = std::move(new_context);
            _surface_resources = std::move(new_surface_resources);
            _vs = std::move(new_vs);
            _ps = std::move(new_ps);
            _sampler = std::move(new_sampler);
            _width = initial_width;
            _height = initial_height;
            _created = true;

            TEIO_DEBUG("consumer created ({}x{}, renderer pid: {})"
                , initial_width, initial_height
                , init_rep.renderer_process_id
            );
            return true;
        }

        void destroy()
        {
            if (!_created && !_device) {
                return;
            }

            if (_context) {
                _context->OMSetRenderTargets(0, nullptr, nullptr);
                _context->Flush();
            }

            _sampler.Reset();
            _ps.Reset();
            _vs.Reset();
            _surface_resources.reset();
            _context.Reset();
            _device.Reset();
            _renderer_process_handle.reset();
            _width = _height = 0;
            _created = false;
        }

        bool sync_latest_frame(uint32_t timeout_ms)
        {
            if (!_created) {
                return false;
            }

            // 획득해둔 KeyedMutex를 사용하여 렌더링 타이밍 동기화 수행
            // (매 프레임 렌더링 전, GL 렌더러 측이 텍스처 쓰기를 완료할 때까지 대기)
            // -> 뮤텍스를 즉시 얻지 못한 경우, GL 렌더러 측에서 아직 작업 중이거나 렌더링된 프레임이 없음을 의미
            //
            // NOTE: AcquireSync 함수 사용 시 단순 성공 여부 판단을 위해 SUCCEEDED 매크로만 사용할 경우,
            //       WAIT_OBJECT_0 반환값을 제외한 나머지 반환 상태값을 제대로 감지하지 못할 수 있음에 유의.
            //       AcquireSync 함수는 다음과 같은 DWORD 상수들을 반환할 수 있다:
            //         - WAIT_OBJECT_0  : KeyedMutex를 성공적으로 획득 -> 렌더링 작업을 계속 진행할 수 있음. (S_OK와 동일한 값)
            //         - WAIT_TIMEOUT   : 지정된 키가 해제되기 전에 타임아웃 간격이 경과했음을 의미.
            //         - WAIT_ABANDONED : SharedSurface와 KeyedMutex가 더 이상 일관된 상태가 아님.
            //                            이 경우, KeyedMutex와 SharedSurface 둘 다 해제한 후 재생성해야 함.
            //       Ref: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync
            constexpr UINT64 kMutexKey = 0;
            switch (const HRESULT sync_hr = _surface_resources->shared_tex_keyed_mutex->AcquireSync(kMutexKey, timeout_ms)) {
            case WAIT_OBJECT_0: // KeyedMutex를 성공적으로 획득했으므로 렌더링 작업 진행 가능
                // Shared 텍스처를 복사본 텍스처로 복사 (락 점유 시간을 최소화하기 위해 별도 텍스처로 데이터를 복사한 뒤 렌더링 수행)
                _context->CopyResource(_surface_resources->copy_tex.Get(), _surface_resources->shared_tex.Get());
                _surface_resources->shared_tex_keyed_mutex->ReleaseSync(kMutexKey); // 텍스처 사용이 끝났으므로 KeyedMutex 잠금 해제
                return true;
            case WAIT_TIMEOUT: // KeyedMutex를 획득하지 못했으므로 렌더링 작업을 건너뜀
                return true; // Continue rendering with the previous frame to maintain smooth presentation
            case WAIT_ABANDONED: // KeyedMutex가 더 이상 일관된 상태가 아님. 이 경우, KeyedMutex와 SharedSurface 둘 다 해제한 후 재생성해야 함.
                TEIO_ERROR("Keyed mutex abandoned - renderer process may have crashed");
                return false;
            default:
                TEIO_ERROR("Unexpected AcquireSync result: 0x{:X}", static_cast<uint32_t>(sync_hr));
                return false;
            }
        }

        void blit_to_render_target(ID3D11RenderTargetView* const target_rtv, const D3D11_VIEWPORT& viewport)
        {
            if (!_created || !target_rtv) {
                return;
            }

            _context->RSSetViewports(1, &viewport);
            _context->OMSetRenderTargets(1, &target_rtv, nullptr);

            _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            _context->IASetInputLayout(nullptr); // No input buffer needed for this VS trick (no vertex buffer; VS uses SV_VertexID)

            _context->VSSetShader(_vs.Get(), nullptr, 0);
            _context->PSSetShader(_ps.Get(), nullptr, 0);
            _context->PSSetShaderResources(0, 1, _surface_resources->copy_tex_srv.GetAddressOf());
            _context->PSSetSamplers(0, 1, _sampler.GetAddressOf());

            _context->Draw(3, 0); // draw a single full-screen triangle
        }

        bool resize(ipc_client& cli, const int32_t new_width, const int32_t new_height)
        {
            if (!_created) {
                TEIO_ERROR("resize called before create");
                return false;
            }

            HANDLE new_surface_handle = nullptr; // new shared surface handle
            if (!this->_request_resize(cli, new_width, new_height, new_surface_handle)) {
                return false;
            }

            // Build the new shared-surface resources; only swap on full success.
            auto new_surface_resources = surface_resources::build(
                _device.Get(), 
                _renderer_process_handle.get(), 
                new_surface_handle
            );
            if (!new_surface_resources) {
                return false;
            }

            // First, need to clear the render target and shader resource views
            // before resizing the swap chain and shared texture.
            _context->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11ShaderResourceView* nullSRV = nullptr;
            _context->PSSetShaderResources(0, 1, &nullSRV);
            _context->Flush(); // Wait for GPU to finish processing

            _surface_resources = std::move(new_surface_resources);
            _width = new_width;
            _height = new_height;

            TEIO_DEBUG("consumer resized to {}x{}", new_width, new_height);
            return true;
        }

    private:
        // The renderer's shared-surface resources, rebuilt together on create/resize.
        struct surface_resources
        {
            ComPtr<ID3D11Texture2D> shared_tex; // renderer's shared surface, opened from its NT handle
            ComPtr<IDXGIKeyedMutex> shared_tex_keyed_mutex; // keyed mutex of `shared_tex` (renderer<->consumer sync)
            ComPtr<ID3D11Texture2D> copy_tex; // private (non-shared) copy of `shared_tex`, sampled by the blit
            ComPtr<ID3D11ShaderResourceView> copy_tex_srv; // shader resource view of `copy_tex`

            surface_resources(const surface_resources&) = delete;
            surface_resources& operator=(const surface_resources&) = delete;

            // Open the renderer's shared surface and build the sampling copy + SRV.
            // Returns nullptr on failure.
            static std::unique_ptr<surface_resources> build(
                ID3D11Device2* const device,
                const HANDLE owner_process,
                const HANDLE surface_handle)
            {
                // make_unique can't reach the private constructor
                std::unique_ptr<surface_resources> out{ new surface_resources{} };

                // Open shared interop texture from native handle
                out->shared_tex = open_shared_texture(device, surface_handle, owner_process);
                if (!out->shared_tex) {
                    TEIO_ERROR("Failed to open surface handle");
                    return nullptr;
                }

                // Get the KeyedMutex interface from the shared texture
                if (FAILED(out->shared_tex.As(&out->shared_tex_keyed_mutex))) {
                    TEIO_ERROR("Failed to get KeyedMutex from shared texture");
                    return nullptr;
                }

                // Create copy of the shared screen texture (non-shared)
                // (This texture will be sampled to present to the render target)
                D3D11_TEXTURE2D_DESC sharedTexCopyDesc{};
                out->shared_tex->GetDesc(&sharedTexCopyDesc); // frame size & format will be same as shared texture
                sharedTexCopyDesc.MipLevels = 1;
                sharedTexCopyDesc.ArraySize = 1;
                sharedTexCopyDesc.SampleDesc.Count = 1;
                sharedTexCopyDesc.SampleDesc.Quality = 0;
                sharedTexCopyDesc.Usage = D3D11_USAGE_DEFAULT;
                sharedTexCopyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                sharedTexCopyDesc.CPUAccessFlags = 0; // No cpu access
                sharedTexCopyDesc.MiscFlags = 0; // No misc flags
                if (FAILED(device->CreateTexture2D(&sharedTexCopyDesc, nullptr, &out->copy_tex))) {
                    TEIO_ERROR("Failed to create shared texture copy");
                    return nullptr;
                }

                // Create Shader Resource View (SRV) of the copied shared screen texture for shader access
                // Equivalent of: `glBindTexture`+ `sampler2D`
                if (FAILED(device->CreateShaderResourceView(out->copy_tex.Get(), nullptr, &out->copy_tex_srv))) {
                    TEIO_ERROR("Failed to create shader resource view");
                    return nullptr;
                }

                return out;
            }

        private:
            surface_resources() = default;
        }; // struct

        bool _request_init(
            ipc_client& cli, 
            const int32_t initial_width, 
            const int32_t initial_height, 
            proto::packets::init_response_t& out)
        {
            // 초기화 요청 전송
            packet_builder<proto::packets::init_request_t> req{ proto::packet_type::init_request };
            req.body()->magic = proto::PROTO_MAGIC;
            req.body()->proto_version = proto::PROTO_VERSION;
            req.body()->frame_width = initial_width;
            req.body()->frame_height = initial_height;

            constexpr std::chrono::seconds init_request_timeout{ 30 };

            std::vector<uint8_t> rep_bytes;
            if (std::errc{} != cli.send_request_sync(req.data(), req.size(), rep_bytes, init_request_timeout)) {
                TEIO_ERROR("failed to send init request");
                return false;
            }

            packet_view view{ rep_bytes.data(), rep_bytes.size() };
            const auto* init_rep = view.body<proto::packets::init_response_t>();
            if (!init_rep || init_rep->status != proto::packets::init_status::ok) {
                TEIO_ERROR("init request rejected by renderer (status: {})"
                    , init_rep ? static_cast<int>(init_rep->status) : -1);
                return false;
            }

            out = *init_rep;
            TEIO_DEBUG("init response (pid: {}, adapter: {:x}-{:x}, surface: {:p})"
                , out.renderer_process_id
                , out.target_adapter_luid.HighPart
                , out.target_adapter_luid.LowPart
                , out.surface_handle
            );
            return true;
        }

        bool _request_resize(
            ipc_client& cli, 
            const int32_t new_width, 
            const int32_t new_height, 
            HANDLE& out_surface_handle)
        {
            packet_builder<proto::packets::frame_resize_request_t> req{ proto::packet_type::frame_resize_request };
            req.body()->width = new_width;
            req.body()->height = new_height;

            std::vector<uint8_t> rep_bytes;
            if (std::errc{} != cli.send_request_sync(req.data(), req.size(), rep_bytes)) {
                TEIO_ERROR("failed to send resize request");
                return false;
            }

            packet_view view{ rep_bytes.data(), rep_bytes.size() };
            const auto* resize_rep = view.body<proto::packets::frame_resize_response_t>();
            if (!resize_rep) {
                TEIO_ERROR("malformed resize response");
                return false;
            }

            out_surface_handle = resize_rep->surface_handle;
            return true;
        }

    private:
        bool _created{ false }; // whether create() has succeeded
        surface_render_options _config{}; // blit options (Y-flip / channel swap)

        unique_handle _renderer_process_handle; // renderer process, used to duplicate shared-surface handles

        ComPtr<ID3D11Device2> _device; // device created on the renderer's adapter
        ComPtr<ID3D11DeviceContext2> _context; // immediate context of `_device`

        // Renderer shared surface + its sampling copy/SRV (null until built).
        // Rebuilt as a unit on resize; non-null iff `_created`.
        std::unique_ptr<surface_resources> _surface_resources;

        ComPtr<ID3D11VertexShader> _vs; // full-screen-triangle vertex shader
        ComPtr<ID3D11PixelShader> _ps; // blit pixel shader (Y-flip / channel swap)
        ComPtr<ID3D11SamplerState> _sampler; // linear-clamp sampler for the blit

        int32_t _width{ 0 }; // current frame width
        int32_t _height{ 0 }; // current frame height
    };

    shared_surface_consumer::shared_surface_consumer()
        : _imp{ std::make_unique<impl>() }
    {}

    shared_surface_consumer::~shared_surface_consumer() = default;

    ID3D11Device2* shared_surface_consumer::get_dx11_device() const noexcept { return _imp->get_dx11_device(); }
    ID3D11DeviceContext2* shared_surface_consumer::get_dx11_context() const noexcept { return _imp->get_dx11_context(); }
    int32_t shared_surface_consumer::get_width() const noexcept { return _imp->get_width(); }
    int32_t shared_surface_consumer::get_height() const noexcept { return _imp->get_height(); }

    bool shared_surface_consumer::is_created() const noexcept { return _imp->is_created(); }

    bool shared_surface_consumer::create(ipc_client& cli, int32_t initial_width, int32_t initial_height, const surface_render_options& config)
    {
        return _imp->create(cli, initial_width, initial_height, config);
    }

    void shared_surface_consumer::destroy() { _imp->destroy(); }

    bool shared_surface_consumer::sync_latest_frame(uint32_t timeout_ms) { return _imp->sync_latest_frame(timeout_ms); }

    void shared_surface_consumer::blit_to_render_target(ID3D11RenderTargetView* target_rtv, const D3D11_VIEWPORT& viewport)
    {
        _imp->blit_to_render_target(target_rtv, viewport);
    }

    bool shared_surface_consumer::resize(ipc_client& cli, int32_t new_width, int32_t new_height)
    {
        return _imp->resize(cli, new_width, new_height);
    }

} // namespace triengine_interop::surface::detail
