#pragma once
#include "common.hpp"
// NGX may alter every shader stage. Preserve the whole immediate-context state,
// without SwapDeviceContextState (which changes the runtime's dispatch tables).
struct PipelineState {
    ID3D11DeviceContext* c;
    struct Bindings {
        ID3D11Buffer* cb[14]{};ID3D11ShaderResourceView* srv[128]{};ID3D11SamplerState* samplers[16]{};
        ~Bindings(){for(auto x:cb)if(x)x->Release();for(auto x:srv)if(x)x->Release();for(auto x:samplers)if(x)x->Release();}
    } stage[6];
#define DECLARE_STAGE(name,type) ComPtr<type> name;ID3D11ClassInstance* name##Classes[256]{};UINT name##Count=256;
    DECLARE_STAGE(vs,ID3D11VertexShader) DECLARE_STAGE(hs,ID3D11HullShader) DECLARE_STAGE(ds,ID3D11DomainShader)
    DECLARE_STAGE(gs,ID3D11GeometryShader) DECLARE_STAGE(ps,ID3D11PixelShader) DECLARE_STAGE(cs,ID3D11ComputeShader)
#undef DECLARE_STAGE
    ComPtr<ID3D11InputLayout> layout;D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11Buffer* vertex[32]{};UINT strides[32]{},offsets[32]{};ComPtr<ID3D11Buffer> index;DXGI_FORMAT indexFormat;UINT indexOffset=0;
    ID3D11RenderTargetView* rt[8]{};ComPtr<ID3D11DepthStencilView> depth;
    ID3D11UnorderedAccessView* omUAV[8]{};ID3D11UnorderedAccessView* csUAV[8]{};
    ComPtr<ID3D11BlendState> blend;float blendFactor[4]{};UINT sampleMask=0;
    ComPtr<ID3D11DepthStencilState> depthState;UINT stencil=0;ComPtr<ID3D11RasterizerState> raster;
    D3D11_VIEWPORT viewports[16]{};UINT viewportCount=16;D3D11_RECT scissors[16]{};UINT scissorCount=16;
    PipelineState(ID3D11DeviceContext* context):c(context){
#define CAPTURE_STAGE(prefix,shader,n) c->prefix##GetShader(&shader,shader##Classes,&shader##Count);c->prefix##GetConstantBuffers(0,14,stage[n].cb);c->prefix##GetShaderResources(0,128,stage[n].srv);c->prefix##GetSamplers(0,16,stage[n].samplers);
        CAPTURE_STAGE(VS,vs,0) CAPTURE_STAGE(HS,hs,1) CAPTURE_STAGE(DS,ds,2) CAPTURE_STAGE(GS,gs,3) CAPTURE_STAGE(PS,ps,4) CAPTURE_STAGE(CS,cs,5)
#undef CAPTURE_STAGE
        c->IAGetInputLayout(&layout);c->IAGetPrimitiveTopology(&topology);c->IAGetVertexBuffers(0,32,vertex,strides,offsets);c->IAGetIndexBuffer(&index,&indexFormat,&indexOffset);
        c->OMGetRenderTargets(8,rt,&depth);c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,omUAV);c->CSGetUnorderedAccessViews(0,8,csUAV);
        c->OMGetBlendState(&blend,blendFactor,&sampleMask);c->OMGetDepthStencilState(&depthState,&stencil);c->RSGetState(&raster);c->RSGetViewports(&viewportCount,viewports);c->RSGetScissorRects(&scissorCount,scissors);
    }
    ~PipelineState(){
        // Unbind NGX's outputs before putting the game's reads back.
        ID3D11UnorderedAccessView* empty[8]{};UINT counts[8];std::fill_n(counts,8,0xffffffff);
        c->CSSetUnorderedAccessViews(0,8,empty,counts);c->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,empty,counts);
        UINT rtCount=0;for(UINT i=0;i<8;i++)if(rt[i])rtCount=i+1;
        c->OMSetRenderTargetsAndUnorderedAccessViews(rtCount,rt,depth.Get(),rtCount,8-rtCount,omUAV+rtCount,counts);
        c->CSSetUnorderedAccessViews(0,8,csUAV,counts);c->OMSetBlendState(blend.Get(),blendFactor,sampleMask);c->OMSetDepthStencilState(depthState.Get(),stencil);
        c->RSSetState(raster.Get());c->RSSetViewports(viewportCount,viewports);c->RSSetScissorRects(scissorCount,scissors);
        c->IASetInputLayout(layout.Get());c->IASetPrimitiveTopology(topology);c->IASetVertexBuffers(0,32,vertex,strides,offsets);c->IASetIndexBuffer(index.Get(),indexFormat,indexOffset);
#define RESTORE_STAGE(prefix,shader,n) c->prefix##SetShader(shader.Get(),shader##Classes,shader##Count);c->prefix##SetConstantBuffers(0,14,stage[n].cb);c->prefix##SetShaderResources(0,128,stage[n].srv);c->prefix##SetSamplers(0,16,stage[n].samplers);for(UINT i=0;i<shader##Count;i++)if(shader##Classes[i])shader##Classes[i]->Release();
        RESTORE_STAGE(VS,vs,0) RESTORE_STAGE(HS,hs,1) RESTORE_STAGE(DS,ds,2) RESTORE_STAGE(GS,gs,3) RESTORE_STAGE(PS,ps,4) RESTORE_STAGE(CS,cs,5)
#undef RESTORE_STAGE
        for(auto x:vertex)if(x)x->Release();for(auto x:rt)if(x)x->Release();for(auto x:omUAV)if(x)x->Release();for(auto x:csUAV)if(x)x->Release();
    }
};
