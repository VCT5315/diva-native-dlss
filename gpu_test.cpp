#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
using Microsoft::WRL::ComPtr;
LRESULT CALLBACK WindowProc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int main(int argc,char** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    HMODULE proxy=nullptr;if(GetFileAttributesW(L".\\dxgi.dll")!=INVALID_FILE_ATTRIBUTES)proxy=LoadLibraryW(L".\\dxgi.dll");printf("ReShade proxy=%p\n",proxy);
    bool testOffscreen=argc>1&&strstr(argv[1],"offscreen")!=nullptr;
    bool testUIDepth=argc>1&&strstr(argv[1],"uidepth")!=nullptr;
    bool testMotion=argc>1&&strstr(argv[1],"motion")!=nullptr;
    bool testStrip=argc>1&&strstr(argv[1],"strip")!=nullptr;
    bool testInstanced=argc>1&&strstr(argv[1],"instanced")!=nullptr;
    bool testPause=argc>1&&strstr(argv[1],"pause")!=nullptr;
    bool testStatus=argc>1&&strstr(argv[1],"status")!=nullptr;
    bool testKeys=argc>1&&strstr(argv[1],"keys")!=nullptr;
    bool testOverlay=argc>1&&strstr(argv[1],"overlay")!=nullptr;
    WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.lpszClassName=L"DivaDLSSGpuTest";wc.hInstance=GetModuleHandleW(nullptr);RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"Diva DLSS GPU Test",WS_OVERLAPPEDWINDOW,0,0,1280,720,nullptr,nullptr,wc.hInstance,nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};sd.BufferDesc.Width=1280;sd.BufferDesc.Height=720;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=1;sd.OutputWindow=window;sd.SampleDesc.Count=1;sd.Windowed=TRUE;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> c;ComPtr<IDXGISwapChain> swap;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&device,nullptr,&c);printf("device=%08X\n",hr);if(FAILED(hr))return 1;
    HMODULE mod=LoadLibraryA(argc>2?argv[2]:".\\DivaNativeDLSS.dll");if(!mod){printf("mod load=%lu\n",GetLastError());return 2;}
    reinterpret_cast<void(*)()>(GetProcAddress(mod,"Init"))();
    auto initialize=reinterpret_cast<void(*)(IDXGISwapChain*,ID3D11Device*,ID3D11DeviceContext*)>(GetProcAddress(mod,"D3DInit"));
    auto onFrame=reinterpret_cast<void(*)(IDXGISwapChain*)>(GetProcAddress(mod,"OnFrame"));
    auto onResize=reinterpret_cast<void(*)(IDXGISwapChain*)>(GetProcAddress(mod,"OnResize"));
    if(!initialize||!onFrame||!onResize)return 10;
    initialize(swap.Get(),device.Get(),c.Get());
    ComPtr<ID3DUserDefinedAnnotation> annotation;c.As(&annotation);auto beginUI=[&](){annotation->BeginEvent(L"pass_sprite");annotation->BeginEvent(L"nested UI draw");};
    auto endUI=[&](){annotation->EndEvent();annotation->EndEvent();};
    if(!annotation)return 21;
    const char* source="cbuffer Offset:register(b0){float4 offset;}struct I{float3 p:POSITION;float3 n:COLOR;};struct O{float4 p:SV_Position;};O VS(I i,uint instance:SV_InstanceID){O o;o.p=float4(i.p,1)+offset+float4(instance*.02,0,0,0);return o;}float4 PS(O i):SV_Target{return float4(0.2,0.4,0.6,1);}";
    ComPtr<ID3DBlob> vsCode,psCode;D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vsCode,nullptr);D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&psCode,nullptr);
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs);device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps);
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;device->CreateInputLayout(elements,2,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout);
    float vertices[]={-.7f,-.7f,.5f,0,0,1,0,.7f,.5f,0,0,1,.7f,-.7f,.5f,0,0,1,.7f,.7f,.5f,0,0,1};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(vertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA data{vertices,0,0};ComPtr<ID3D11Buffer> vb;device->CreateBuffer(&bd,&data,&vb);
    bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> cb;device->CreateBuffer(&bd,nullptr,&cb);
    unsigned short indices[]={0,1,2,3};bd.ByteWidth=sizeof(indices);bd.BindFlags=D3D11_BIND_INDEX_BUFFER;data.pSysMem=indices;ComPtr<ID3D11Buffer> ib;device->CreateBuffer(&bd,&data,&ib);
    const char* uiSource="struct I{float3 p:POSITION;float3 c:COLOR;};struct O{float4 p:SV_Position;float3 c:COLOR;};O VS(I i){O o;o.p=float4(i.p,1);o.c=i.c;return o;}float4 PS(O i):SV_Target{return float4(i.c,1);}";
    ComPtr<ID3DBlob> uiVSCode,uiPSCode;D3DCompile(uiSource,strlen(uiSource),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&uiVSCode,nullptr);D3DCompile(uiSource,strlen(uiSource),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&uiPSCode,nullptr);
    ComPtr<ID3D11VertexShader> uiVS;ComPtr<ID3D11PixelShader> uiPS;ComPtr<ID3D11InputLayout> uiLayout;device->CreateVertexShader(uiVSCode->GetBufferPointer(),uiVSCode->GetBufferSize(),nullptr,&uiVS);device->CreatePixelShader(uiPSCode->GetBufferPointer(),uiPSCode->GetBufferSize(),nullptr,&uiPS);
    elements[1].SemanticName="COLOR";device->CreateInputLayout(elements,2,uiVSCode->GetBufferPointer(),uiVSCode->GetBufferSize(),&uiLayout);
    float uiVertices[]={-1,1,0,1,0,0,-.6f,1,0,1,0,0,-1,.6f,0,1,0,0,-.6f,1,0,1,0,0,-.6f,.6f,0,1,0,0,-1,.6f,0,1,0,0};
    bd.ByteWidth=sizeof(uiVertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;data.pSysMem=uiVertices;ComPtr<ID3D11Buffer> uiVB;device->CreateBuffer(&bd,&data,&uiVB);
    ComPtr<ID3D11Texture2D> sceneColor;
    ComPtr<ID3D11Texture2D> back,depth,stage;swap->GetBuffer(0,IID_PPV_ARGS(&back));ComPtr<ID3D11RenderTargetView> rtv;device->CreateRenderTargetView(back.Get(),nullptr,&rtv);
    D3D11_TEXTURE2D_DESC td{};td.Width=1280;td.Height=720;td.MipLevels=1;td.ArraySize=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;device->CreateTexture2D(&td,nullptr,&depth);ComPtr<ID3D11DepthStencilView> dsv;device->CreateDepthStencilView(depth.Get(),nullptr,&dsv);
    back->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;device->CreateTexture2D(&td,nullptr,&stage);
    const auto makeScene=[&](){
        if(!testOffscreen)return;
        D3D11_TEXTURE2D_DESC sceneDesc{};back->GetDesc(&sceneDesc);sceneDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&sceneDesc,nullptr,&sceneColor);rtv.Reset();device->CreateRenderTargetView(sceneColor.Get(),nullptr,&rtv);
    };makeScene();
    UINT testWidth=1280,testHeight=720;
    ComPtr<ID3D11Texture2D> frozenRaw; unsigned char expectedNative[4]{};
    for(int frame=0;frame<24;frame++){
        if(frame==12){
            c->ClearState();sceneColor.Reset();rtv.Reset();back.Reset();stage.Reset();dsv.Reset();depth.Reset();
            testWidth=1600;testHeight=900;hr=swap->ResizeBuffers(1,testWidth,testHeight,DXGI_FORMAT_R8G8B8A8_UNORM,0);if(FAILED(hr)){printf("FAIL ResizeBuffers=%08X\n",hr);return 8;}
            onResize(swap.Get());
            swap->GetBuffer(0,IID_PPV_ARGS(&back));device->CreateRenderTargetView(back.Get(),nullptr,&rtv);
            td.Width=testWidth;td.Height=testHeight;td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;device->CreateTexture2D(&td,nullptr,&depth);device->CreateDepthStencilView(depth.Get(),nullptr,&dsv);
            back->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;device->CreateTexture2D(&td,nullptr,&stage);makeScene();
        }
        float offset[]={-.1f+frame*.02f,0,0,0};c->UpdateSubresource(cb.Get(),0,nullptr,offset,0,0);
        float clear[]={0,0,0,1};c->ClearRenderTargetView(rtv.Get(),clear);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        auto target=rtv.Get();c->OMSetRenderTargets(1,&target,dsv.Get());D3D11_VIEWPORT vp{0,0,float(testWidth),float(testHeight),0,1};c->RSSetViewports(1,&vp);c->RSSetState(nullptr);
        c->IASetInputLayout(layout.Get());c->IASetPrimitiveTopology(testStrip?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=24,zero=0;auto buffer=vb.Get();c->IASetVertexBuffers(0,1,&buffer,&stride,&zero);
        c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);auto constant=cb.Get();c->VSSetConstantBuffers(0,1,&constant);c->PSSetConstantBuffers(0,1,&constant);
        c->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);if(testInstanced)c->DrawIndexedInstanced(testStrip?4:3,2,0,0,2);else c->DrawIndexed(testStrip?4:3,0,0);
        ComPtr<ID3D11Buffer> saved;c->PSGetConstantBuffers(0,1,&saved);if(saved.Get()!=cb.Get()){printf("FAIL state restoration\n");return 3;}
        UINT vpCount=1;D3D11_VIEWPORT restored{};c->RSGetViewports(&vpCount,&restored);
        if(vpCount!=1||restored.TopLeftX!=0||restored.TopLeftY!=0||restored.Width!=vp.Width||restored.Height!=vp.Height){printf("FAIL viewport restoration\n");return 11;}
        {
            beginUI();c->OMSetRenderTargets(1,&target,testUIDepth?dsv.Get():nullptr);c->IASetInputLayout(uiLayout.Get());buffer=uiVB.Get();c->IASetVertexBuffers(0,1,&buffer,&stride,&zero);c->VSSetShader(uiVS.Get(),nullptr,0);c->PSSetShader(uiPS.Get(),nullptr,0);c->Draw(6,0);endUI();
        }
        if(sceneColor)c->CopyResource(back.Get(),sceneColor.Get());
        if(testPause&&frame==23){back->GetDesc(&td);td.BindFlags=0;device->CreateTexture2D(&td,nullptr,&frozenRaw);c->CopyResource(frozenRaw.Get(),back.Get());}
        onFrame(swap.Get());
        
        if(frame==8||frame==20){c->CopyResource(stage.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE mapped{};hr=c->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped);if(FAILED(hr))return 5;
            auto pixel=static_cast<unsigned char*>(mapped.pData)+(testHeight/2)*mapped.RowPitch+(testWidth/2)*4;printf("center[%ux%u]=%u,%u,%u,%u\n",testWidth,testHeight,pixel[0],pixel[1],pixel[2],pixel[3]);
            int expected=int((.5f-float(testWidth)*.01f/64.f)*255.f);
            bool good=!testMotion||(abs(int(pixel[0])-expected)<=2&&pixel[1]>=125&&pixel[1]<=130);
            {auto uiPixel=static_cast<unsigned char*>(mapped.pData)+20*mapped.RowPitch+20*4;printf("protected UI=%u,%u,%u,%u\n",uiPixel[0],uiPixel[1],uiPixel[2],uiPixel[3]);good&=testStatus||(uiPixel[0]==255&&uiPixel[1]==0&&uiPixel[2]==0);}
            if(testStatus&&frame==8){
                FILE* picture=nullptr;fopen_s(&picture,"status-preview.ppm","wb");if(!picture)return 12;fprintf(picture,"P6\n%u %u\n255\n",testWidth,testHeight);
                for(UINT y=0;y<testHeight;++y)for(UINT x=0;x<testWidth;++x){auto p=static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;fwrite(p,1,3,picture);}fclose(picture);
                auto p=static_cast<unsigned char*>(mapped.pData)+30*mapped.RowPitch+30*4;
                if(p[0]>90){printf("FAIL status badge did not render\n");good=false;}
            }
            if(testMotion){
                const UINT points[5][2]={{testWidth-20,20},{20,testHeight-20},{testWidth-20,testHeight-20},{testWidth/2,20},{testWidth/2,testHeight-20}};
                for(const auto& point:points){auto p=static_cast<unsigned char*>(mapped.pData)+point[1]*mapped.RowPitch+point[0]*4;
                    good&=abs(int(p[0])-128)<=1&&abs(int(p[1])-128)<=1&&abs(int(p[2])-128)<=1;
                    printf("full-screen sample (%u,%u)=%u,%u,%u\n",point[0],point[1],p[0],p[1],p[2]);}
            }
            c->Unmap(stage.Get(),0);if(!good){printf("FAIL motion / native UI preservation\n");return 6;}
        }
        if(testPause&&frame==23){c->CopyResource(stage.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE m{};c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m);memcpy(expectedNative,static_cast<unsigned char*>(m.pData)+(testHeight/2)*m.RowPitch+(testWidth*65/100)*4,4);c->Unmap(stage.Get(),0);}
        hr=swap->Present(0,0);Sleep(100);if(FAILED(hr)){printf("present=%08X\n",hr);return 4;}
    }
    if(testPause){
        unsigned char expected[4]{};UINT sampleX=testWidth*65/100,sampleY=testHeight/2;
        c->CopyResource(stage.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE map{};c->Map(stage.Get(),0,D3D11_MAP_READ,0,&map);
        memcpy(expected,expectedNative,4);c->Unmap(stage.Get(),0);
        for(int paused=0;paused<5;++paused){
            c->CopyResource(back.Get(),frozenRaw.Get());
            for(int v=0;v<6;++v){uiVertices[v*6+3]=0;uiVertices[v*6+4]=1;uiVertices[v*6+5]=0;}c->UpdateSubresource(uiVB.Get(),0,nullptr,uiVertices,0,0);
            beginUI();auto target=rtv.Get();c->OMSetRenderTargets(1,&target,nullptr);c->OMSetDepthStencilState(nullptr,0);c->OMSetBlendState(nullptr,nullptr,0xffffffff);
            D3D11_VIEWPORT vp{0,0,float(testWidth),float(testHeight),0,1};c->RSSetViewports(1,&vp);c->RSSetState(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->IASetInputLayout(uiLayout.Get());UINT stride=24,zero=0;auto buffer=uiVB.Get();c->IASetVertexBuffers(0,1,&buffer,&stride,&zero);c->VSSetShader(uiVS.Get(),nullptr,0);c->PSSetShader(uiPS.Get(),nullptr,0);c->Draw(6,0);endUI();
            onFrame(swap.Get());c->CopyResource(stage.Get(),back.Get());c->Map(stage.Get(),0,D3D11_MAP_READ,0,&map);
            auto p=static_cast<unsigned char*>(map.pData)+sampleY*map.RowPitch+sampleX*4;bool good=true;for(int i=0;i<4;++i)good&=abs(int(p[i])-expected[i])<=1;
            auto ui=static_cast<unsigned char*>(map.pData)+20*map.RowPitch+20*4;good&=ui[0]==0&&ui[1]==255&&ui[2]==0;
            printf("paused frame=%d pixel=%u,%u,%u,%u new UI=%u,%u,%u\n",paused,p[0],p[1],p[2],p[3],ui[0],ui[1],ui[2]);c->Unmap(stage.Get(),0);
            if(!good){printf("FAIL pause retention / changed UI\n");return 13;}
            swap->Present(0,0);
        }
        float blue[]={0,0,1,1};c->ClearRenderTargetView(rtv.Get(),blue);onFrame(swap.Get());c->CopyResource(stage.Get(),back.Get());c->Map(stage.Get(),0,D3D11_MAP_READ,0,&map);
        auto p=static_cast<unsigned char*>(map.pData)+sampleY*map.RowPitch+sampleX*4;bool clear=p[0]==0&&p[1]==0&&p[2]==255;c->Unmap(stage.Get(),0);if(!clear){printf("FAIL scene change reused stale output\n");return 14;}
        printf("paused output preserved, new UI preserved, scene change bypassed\n");
    }
    if(testKeys){
        wchar_t modPath[MAX_PATH]{};GetModuleFileNameW(mod,modPath,MAX_PATH);wchar_t* slash=wcsrchr(modPath,L'\\');if(!slash)return 15;*(slash+1)=0;std::wstring ini=std::wstring(modPath)+L"settings.ini";
        SendMessageW(window,WM_KEYDOWN,VK_F4,1);SendMessageW(window,WM_KEYUP,VK_F4,LPARAM(1)|(LPARAM(1)<<30)|(LPARAM(1)<<31));onFrame(swap.Get());
        if(GetPrivateProfileIntW(L"DLSS",L"Enable",99,ini.c_str())!=0){printf("FAIL short press was missed\n");return 16;}
        for(int repeated=0;repeated<10;++repeated){SendMessageW(window,WM_KEYDOWN,VK_F4,LPARAM(1)|(LPARAM(1)<<30));onFrame(swap.Get());}
        if(GetPrivateProfileIntW(L"DLSS",L"Enable",99,ini.c_str())!=0){printf("FAIL repeated key toggled\n");return 17;}
        SendMessageW(window,WM_KEYUP,VK_F4,LPARAM(1)|(LPARAM(1)<<30)|(LPARAM(1)<<31));SendMessageW(window,WM_KEYDOWN,VK_F4,1);onFrame(swap.Get());
        if(GetPrivateProfileIntW(L"DLSS",L"Enable",99,ini.c_str())!=1){printf("FAIL next press did not toggle\n");return 18;}
        printf("short press queued; 10 repeated key events ignored; next press toggled once; settings persisted\n");
    }
    if(testOverlay){
        wchar_t modPath[MAX_PATH]{};GetModuleFileNameW(mod,modPath,MAX_PATH);wchar_t* slash=wcsrchr(modPath,L'\\');if(!slash)return 15;*(slash+1)=0;std::wstring ini=std::wstring(modPath)+L"settings.ini";
        auto overlayVisible=reinterpret_cast<int(*)()>(GetProcAddress(mod,"DivaNativeDLSSOverlayVisible"));
        if(!overlayVisible){printf("FAIL overlay export missing\n");return 30;}
        // F7 opens the panel.
        SendMessageW(window,WM_KEYDOWN,VK_F7,1);SendMessageW(window,WM_KEYUP,VK_F7,LPARAM(1)|(LPARAM(1)<<30)|(LPARAM(1)<<31));
        // A few frames so the panel is drawn and blended over the scene.
        for(int i=0;i<3;++i){
            float offset[]={0,0,0,0};c->UpdateSubresource(cb.Get(),0,nullptr,offset,0,0);
            float clearScene[]={0,0,0,1};c->ClearRenderTargetView(rtv.Get(),clearScene);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
            auto target=rtv.Get();c->OMSetRenderTargets(1,&target,dsv.Get());
            D3D11_VIEWPORT vp{0,0,float(testWidth),float(testHeight),0,1};c->RSSetViewports(1,&vp);c->RSSetState(nullptr);
            c->IASetInputLayout(layout.Get());c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride=24,zero=0;auto buffer=vb.Get();c->IASetVertexBuffers(0,1,&buffer,&stride,&zero);
            c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);
            auto constant=cb.Get();c->VSSetConstantBuffers(0,1,&constant);c->PSSetConstantBuffers(0,1,&constant);
            c->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);c->DrawIndexed(3,0,0);
            onFrame(swap.Get());
        }
        if(!overlayVisible()){printf("FAIL overlay did not open on F7\n");return 31;}

        // The panel is centred and 560 px wide, so the middle scanline must show
        // panel pixels instead of the scene colour.
        c->CopyResource(stage.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE m{};
        if(FAILED(c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m)))return 32;
        auto* row=static_cast<unsigned char*>(m.pData)+(testHeight/2)*m.RowPitch;
        int replaced=0;
        for(UINT x=0;x<testWidth;++x){const unsigned char* p=row+x*4;if(abs(int(p[0])-51)>10||abs(int(p[1])-102)>10||abs(int(p[2])-153)>10)++replaced;}
        printf("overlay scanline: %d / %u pixels differ from the scene\n",replaced,testWidth);
        c->Unmap(stage.Get(),0);
        if(replaced<200){printf("FAIL overlay panel not visible on screen\n");return 33;}

        // Keyboard navigation must change and persist a setting: DOWN DOWN lands
        // on Sharpness, RIGHT raises it by 5.
        SendMessageW(window,WM_KEYDOWN,VK_DOWN,1);SendMessageW(window,WM_KEYDOWN,VK_DOWN,1);SendMessageW(window,WM_KEYDOWN,VK_RIGHT,1);
        for(int i=0;i<3;++i){onFrame(swap.Get());}
        const int sharpness=GetPrivateProfileIntW(L"FSR",L"Sharpness",-1,ini.c_str());
        printf("panel changed FSR sharpness to %d\n",sharpness);
        if(sharpness!=5){printf("FAIL panel did not persist a setting change\n");return 34;}

        // F7 again closes it.
        SendMessageW(window,WM_KEYDOWN,VK_F7,1);onFrame(swap.Get());
        if(overlayVisible()){printf("FAIL overlay did not close\n");return 35;}
        printf("configuration panel opened, rendered, changed a setting and closed\n");
    }
    printf("GPU removed reason=%08X; state preserved\n",device->GetDeviceRemovedReason());
    auto frames=reinterpret_cast<unsigned long long(*)()>(GetProcAddress(mod,"DivaNativeDLSSSuccessfulFrames"))();printf("DLSS successful published frames=%llu\n",frames);
    // The base loop publishes exactly 24 frames; the overlay mode runs a few
    // more on purpose, so it only has to be at least that many.
    const bool frameCountOK = testMotion || (testOverlay ? frames >= 24 : frames == 24);
    if(!frameCountOK){printf("FAIL: DLSS did not evaluate and publish every frame\n");return 9;}
    printf("render validation complete; forced test-host exit; NGX Shutdown1 not tested\n");
    TerminateProcess(GetCurrentProcess(),FAILED(device->GetDeviceRemovedReason())?7:0);
    return FAILED(device->GetDeviceRemovedReason())?7:0;
}
