#include "pch.h"

#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "thirdparty/TVTestPlugin.h"
#include "resource.h"

using namespace Microsoft::WRL;

// #pragma comment(lib, "ntdll.lib")
// extern "C" ULONG DbgPrint(PCSTR Format, ...);

// メッセージウィンドウ向けメッセージ
#define WM_APP_PACKET (WM_APP + 0)
#define WM_APP_RESIZE (WM_APP + 1)

struct Status
{
    std::wstring url;
    bool receiving;
    bool loading;
};

class CDataBroadcastingWV2 : public TVTest::CTVTestPlugin, TVTest::CTVTestEventHandler
{
    std::wstring iniFile;
    std::wstring resourceDirectory;

    // 半端なので要改善
    static const size_t PACKET_SIZE = 188;
    size_t PACKET_BUFFER_SIZE = PACKET_SIZE * 100;
    int PAKCET_QUEUE_SIZE = 10;
    std::mutex packetBufferLock;
    BYTE* packetBuffer = new BYTE[PACKET_BUFFER_SIZE];
    size_t packetBufferPosition = 0;
    std::atomic<int> packetBufferInQueue = 0;

    HWND hRemoteWnd = nullptr;
    HWND hVideoWnd = nullptr;
    HWND hWebViewWnd = nullptr;
    HWND hContainerWnd = nullptr;
    HWND hMessageWnd = nullptr;
    bool invisible = false;
    RECT videoRect = {};
    TVTest::ServiceInfo currentService = {};
    TVTest::ChannelInfo currentChannel = {};
    std::unordered_set<WORD> pesPIDList;
    Status status;
    virtual bool OnChannelChange();
    virtual bool OnServiceChange();
    virtual bool OnServiceUpdate();
    virtual bool OnCommand(int ID);
    virtual bool OnPluginEnable(bool fEnable);
    virtual void OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo);
    virtual void OnFilterGraphFinalized(TVTest::FilterGraphInfo* pInfo);
    virtual bool OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo);
    virtual bool OnFullscreenChange(bool fFullscreen);

    void RestoreVideoWindow();
    void Tune();
    void InitWebView2();
    std::wstring GetIniItem(const wchar_t* key, const wchar_t* def);

    wil::com_ptr<ICoreWebView2Controller> webViewController;
    wil::com_ptr<ICoreWebView2> webView;

    static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData);
    static INT_PTR CALLBACK RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static BOOL CALLBACK StreamCallback(BYTE* pData, void* pClientData);
    static LRESULT CALLBACK MessageWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK WindowMessageCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, void* pUserData);

public:
    virtual bool GetPluginInfo(TVTest::PluginInfo* pInfo);
    virtual bool Initialize();
    virtual bool Finalize();
};

bool CDataBroadcastingWV2::GetPluginInfo(TVTest::PluginInfo* pInfo)
{
    pInfo->Type = TVTest::PLUGIN_TYPE_NORMAL;
    pInfo->Flags = TVTest::PLUGIN_FLAG_DISABLEONSTART;
    pInfo->pszPluginName = L"TVTDataBroadcastingWV2";
    pInfo->pszCopyright = L"MIT License";
    pInfo->pszDescription = L"データ放送を表示";
    return true;
}

BOOL CALLBACK CDataBroadcastingWV2::StreamCallback(BYTE* pData, void* pClientData)
{
    auto pThis = (CDataBroadcastingWV2*)pClientData;
    if (!pThis->webView)
    {
        return TRUE;
    }
    if (pThis->packetBufferInQueue >= pThis->PAKCET_QUEUE_SIZE)
    {
        return TRUE;
    }
    std::lock_guard<std::mutex> lock(pThis->packetBufferLock);
    memcpy(pThis->packetBuffer + pThis->packetBufferPosition, pData, PACKET_SIZE);
    pThis->packetBufferPosition += PACKET_SIZE;
    if (pThis->packetBufferPosition == pThis->PACKET_BUFFER_SIZE)
    {
        pThis->packetBufferInQueue++;
        PostMessageW(pThis->hMessageWnd, WM_APP_PACKET, (WPARAM)&pThis->packetBufferInQueue, (LPARAM)pThis->packetBuffer);
        pThis->packetBuffer = new BYTE[188 * 100];
        pThis->packetBufferPosition = 0;
    }
    return TRUE;
}

bool CDataBroadcastingWV2::OnServiceChange()
{
    return OnServiceUpdate();
}

bool CDataBroadcastingWV2::OnChannelChange()
{
    return OnServiceUpdate();
}

bool CDataBroadcastingWV2::OnServiceUpdate()
{
    int numServices;
    auto serviceIndex = this->m_pApp->GetService(&numServices);
    if (serviceIndex == -1)
    {
        return true;
    }
    this->m_pApp->GetCurrentChannelInfo(&this->currentChannel);
    this->m_pApp->GetServiceInfo(serviceIndex, &this->currentService);
    pesPIDList.clear();
    for (auto i = 0; i < numServices; i++)
    {
        TVTest::ServiceInfo serviceInfo = { sizeof(TVTest::ServiceInfo) };
        if (this->m_pApp->GetServiceInfo(i, &serviceInfo))
        {
            pesPIDList.insert(serviceInfo.VideoPID);
            for (auto j = 0; j < serviceInfo.NumAudioPIDs; j++)
            {
                pesPIDList.insert(serviceInfo.AudioPID[j]);
            }
        }
    }
    Tune();
    return true;
}

void CDataBroadcastingWV2::RestoreVideoWindow()
{
    RECT r;
    if (GetClientRect(hContainerWnd, &r))
    {
        PostMessageW(hContainerWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right - r.left, r.bottom - r.top));
    }
}

BOOL CALLBACK CDataBroadcastingWV2::WindowMessageCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, void* pUserData)
{
    if (uMsg == WM_SIZE)
    {
        auto pThis = (CDataBroadcastingWV2*)pUserData;
        if (pThis->hVideoWnd && pThis->hMessageWnd)
        {
            if (!pThis->invisible)
            {
                // 無理やり動画ウィンドウを移動させている都合上リサイズ時に位置大きさが初期化されてしまうので一時的に非表示にさせる
                SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
            }
            // 実際に動画ウィンドウの大きさが変わるのはメッセージ処理後なのでPostMessageでやり過ごす
            PostMessageW(pThis->hMessageWnd, WM_APP_RESIZE, 0, 0);
        }
    }
    return FALSE;
}

std::wstring CDataBroadcastingWV2::GetIniItem(const wchar_t* key, const wchar_t* def)
{
    DWORD size = 100;
    std::wstring item(size, 0);
    while (true)
    {
        auto result = GetPrivateProfileStringW(L"TVTDataBroadcastingWV2", key, def, &item[0], size, this->iniFile.c_str());
        if (result + 1 != size)
        {
            item.resize(result);
            return item;
        }
        size *= 2;
        item.reserve(size);
    }
}

bool CDataBroadcastingWV2::Initialize()
{
    DWORD size = 10;
    while (true)
    {
        std::wstring filename(size, 0);
        auto result = GetModuleFileNameW(g_hinstDLL, &filename[0], size);
        if (result == 0)
        {
            break;
        }
        if (result == size)
        {
            size = size * 2;
            filename.reserve(size);
            continue;
        }
        filename.resize(result);
        std::filesystem::path path(filename);
        path.replace_extension();
        resourceDirectory = path;
        path.replace_extension(L".ini");
        iniFile = path;
        break;
    }
    m_pApp->SetEventCallback(EventCallback, this);
    m_pApp->RegisterCommand(IDC_KEY_D, L"DataButton", L"dボタン");
    m_pApp->RegisterCommand(IDC_KEY_UP, L"Up", L"↑");
    m_pApp->RegisterCommand(IDC_KEY_DOWN, L"Down", L"↓");
    m_pApp->RegisterCommand(IDC_KEY_LEFT, L"Left", L"←");
    m_pApp->RegisterCommand(IDC_KEY_RIGHT, L"Right", L"→");
    m_pApp->RegisterCommand(IDC_KEY_0, L"Digit0", L"0");
    m_pApp->RegisterCommand(IDC_KEY_1, L"Digit1", L"1");
    m_pApp->RegisterCommand(IDC_KEY_2, L"Digit2", L"2");
    m_pApp->RegisterCommand(IDC_KEY_3, L"Digit3", L"3");
    m_pApp->RegisterCommand(IDC_KEY_4, L"Digit4", L"4");
    m_pApp->RegisterCommand(IDC_KEY_5, L"Digit5", L"5");
    m_pApp->RegisterCommand(IDC_KEY_6, L"Digit6", L"6");
    m_pApp->RegisterCommand(IDC_KEY_7, L"Digit7", L"7");
    m_pApp->RegisterCommand(IDC_KEY_8, L"Digit8", L"8");
    m_pApp->RegisterCommand(IDC_KEY_9, L"Digit9", L"9");
    m_pApp->RegisterCommand(IDC_KEY_10, L"Digit10", L"10");
    m_pApp->RegisterCommand(IDC_KEY_11, L"Digit11", L"11");
    m_pApp->RegisterCommand(IDC_KEY_12, L"Digit12", L"12");
    m_pApp->RegisterCommand(IDC_KEY_ENTER, L"Enter", L"決定");
    m_pApp->RegisterCommand(IDC_KEY_BACK, L"Back", L"戻る");
    m_pApp->RegisterCommand(IDC_KEY_BLUE, L"BlueButton", L"青");
    m_pApp->RegisterCommand(IDC_KEY_RED, L"RedButton", L"赤");
    m_pApp->RegisterCommand(IDC_KEY_GREEN, L"GreenButton", L"緑");
    m_pApp->RegisterCommand(IDC_KEY_YELLOW, L"YellowButton", L"黄");
    m_pApp->RegisterCommand(IDC_KEY_RELOAD, L"Reload", L"再読み込み");
    m_pApp->RegisterCommand(IDC_KEY_DEVTOOL, L"OpenDevTools", L"開発者ツール");
    m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCEW(IDB_BITMAP1));
    TVTest::StatusItemInfo statusItemInfo = {};
    statusItemInfo.Size = sizeof(statusItemInfo);
    statusItemInfo.ID = 1;
    statusItemInfo.Flags = 0;
    statusItemInfo.Style = TVTest::STATUS_ITEM_STYLE_VARIABLEWIDTH;
    statusItemInfo.pszIDText = L"DataBroadcastingStatus";
    statusItemInfo.pszName = L"データ放送ステータス";
    statusItemInfo.MinWidth = 0;
    statusItemInfo.MaxWidth = -1;
    statusItemInfo.DefaultWidth = TVTest::StatusItemWidthByFontSize(10);
    statusItemInfo.MinHeight = 0;
    m_pApp->RegisterStatusItem(&statusItemInfo);
    return true;
}

bool CDataBroadcastingWV2::Finalize()
{
    this->OnPluginEnable(false);
    if (this->hMessageWnd)
    {
        auto hWnd = this->hMessageWnd;
        DestroyWindow(hWnd);
        this->hMessageWnd = nullptr;
    }
    return true;
}

LRESULT CALLBACK CDataBroadcastingWV2::MessageWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    auto pThis = (CDataBroadcastingWV2*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (!pThis)
    {
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    switch (uMsg)
    {
    case WM_TIMER:
    {
        if (wParam == 1)
        {
            SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            KillTimer(hWnd, wParam);
        }
        break;
    }
    case WM_APP_RESIZE:
    {
        if (pThis->webViewController)
        {
            SetTimer(pThis->hMessageWnd, 1, 50, nullptr);
            RECT rect;
            if (GetClientRect(pThis->hContainerWnd, &rect))
            {
                RECT prev;
                if (SUCCEEDED(pThis->webViewController->get_Bounds(&prev)))
                {
                    if (memcmp(&prev, &rect, sizeof(prev)))
                    {
                        pThis->webViewController->put_Bounds(rect);
                    }
                }
            }
        }
        break;
    }
    case WM_APP_PACKET:
    {
        auto packetBufferInQueue = (std::atomic<int>*)wParam;
        auto buffer = (BYTE*)lParam;
        if (pThis->webView)
        {
            WCHAR head[] = LR"({"type":"stream","data":[)";
            WCHAR tail[] = LR"(]})";
            size_t size = _countof(head) - 1 + pThis->PACKET_BUFFER_SIZE * 4 /* '255,' */ + _countof(tail) + 1;
            auto buf = new WCHAR[size];
            wcscpy_s(buf, size, head);
            size_t pos = 0;
            pos += wcslen(head);
            for (size_t p = 0; p < pThis->PACKET_BUFFER_SIZE; p += PACKET_SIZE)
            {
                // 動画、音声のPESは不要なので削っておく
                // StreamCallbackで削っておきたいけどやや処理が複雑になるのでこっちで
                // 8-bit sync byte
                // 1-bit TEI
                // 1-bit PUSI
                // 1-bit priority
                // 13-bit PID
                auto pid = ((buffer[p + 1] << 8) | buffer[p + 2]) & 0x1fff;
                if (pThis->pesPIDList.find(pid) != pThis->pesPIDList.end())
                {
                    continue;
                }
                for (size_t i = p; i < p + PACKET_SIZE; i++)
                {
                    // デバッグビルドだからかitowとか遅すぎて間に合わないので自前でやる
                    if (buffer[i] < 10)
                    {
                        buf[pos] = L'0' + buffer[i];
                        pos += 1;
                    }
                    else if (buffer[i] < 100)
                    {
                        buf[pos] = L'0' + (buffer[i] / 10);
                        pos += 1;
                        buf[pos] = L'0' + (buffer[i] % 10);
                        pos += 1;
                    }
                    else
                    {
                        buf[pos] = L'0' + (buffer[i] / 100);
                        pos += 1;
                        buf[pos] = L'0' + ((buffer[i] / 10) % 10);
                        pos += 1;
                        buf[pos] = L'0' + (buffer[i] % 10);
                        pos += 1;
                    }
                    buf[pos] = L',';
                    pos += 1;
                }
            }
            pos--;
            wcscpy_s(buf + pos, size - pos, tail);
            auto hr = pThis->webView->PostWebMessageAsJson(buf);
            delete[] buf;
        }
        delete[] buffer;
        (*packetBufferInQueue)--;
        break;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void CDataBroadcastingWV2::OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo)
{
    this->hContainerWnd = FindWindowExW(FindWindowExW(FindWindowExW(this->m_pApp->GetAppWindow(), nullptr, L"TVTest Splitter", nullptr), nullptr, L"TVTest View", nullptr), nullptr, L"TVTest Video Container", nullptr);
    auto hVideoWnd = GetWindow(hContainerWnd, GW_CHILD);
    hVideoWnd = GetWindow(hVideoWnd, GW_HWNDLAST);
    if (hVideoWnd && hVideoWnd == hWebViewWnd)
    {
        hVideoWnd = GetWindow(hVideoWnd, GW_HWNDPREV);
    }
    this->hVideoWnd = hVideoWnd;
}

void CDataBroadcastingWV2::OnFilterGraphFinalized(TVTest::FilterGraphInfo* pInfo)
{
    this->hVideoWnd = nullptr;
}

std::string wstrToUTF8String(const wchar_t* ws)
{
    auto size = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring utf8StrToWString(const char* s)
{
    auto size = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &result[0], size);
    return result;
}

void CDataBroadcastingWV2::InitWebView2()
{
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        env->CreateCoreWebView2Controller(this->hContainerWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [env, this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (controller != nullptr) {
                this->webViewController = controller;
                this->webViewController->get_CoreWebView2(this->webView.put());
            }
            auto hWebViewWnd = FindWindowExW(this->hContainerWnd, nullptr, L"Chrome_WidgetWin_0", nullptr);
            this->hWebViewWnd = hWebViewWnd;
            // 動画ウィンドウといい感じに合成させるために必要 (Windows 8以降じゃないと動かないはず)
            SetWindowLongW(hWebViewWnd, GWL_EXSTYLE, GetWindowLongW(hWebViewWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
            auto controller2 = this->webViewController.query<ICoreWebView2Controller2>();
            COREWEBVIEW2_COLOR c = { };
            auto ff = controller2->put_DefaultBackgroundColor(c);
            wil::com_ptr<ICoreWebView2Settings> settings;
            this->webView->get_Settings(settings.put());
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            settings->put_IsWebMessageEnabled(TRUE);

            RECT bounds;
            GetClientRect(this->hContainerWnd, &bounds);
            this->webViewController->put_Bounds(bounds);

            EventRegistrationToken token;
            auto webView3 = this->webView.query<ICoreWebView2_3>();
            auto resourceDirectory = this->GetIniItem(L"ResourceDirectory", this->resourceDirectory.c_str());
            webView3->SetVirtualHostNameToFolderMapping(L"TVTDataBroadcastingWV2.invalid", resourceDirectory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

            // 仮想ホスト以外へのリクエストは全てブロックする
            webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            this->webView->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                HRESULT hr = args->get_Request(request.put());
                if (FAILED(hr))
                {
                    return hr;
                }
                wil::unique_cotaskmem_string uri;
                hr = request->get_Uri(uri.put());
                if (FAILED(hr))
                {
                    return hr;
                }
                if (_wcsnicmp(L"https://TVTDataBroadcastingWV2.invalid/", uri.get(), wcslen(L"https://TVTDataBroadcastingWV2.invalid/")))
                {
                    wil::com_ptr<ICoreWebView2Environment> env;
                    hr = this->webView.query<ICoreWebView2_2>()->get_Environment(env.put());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                    hr = env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", response.put());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    return args->put_Response(response.get());
                }
                return S_OK;
            }).Get(), &token);

            this->webView->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, hWebViewWnd](ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                if (SUCCEEDED(args->get_WebMessageAsJson(message.put())))
                {
                    auto messageUTF8 = wstrToUTF8String(message.get());

                    auto a = nlohmann::json::parse(messageUTF8);
                    auto type = a["type"].get<std::string>();
                    if (type == "videoChanged")
                    {
                        auto left = a["left"].get<int>();
                        auto right = a["right"].get<int>();
                        auto bottom = a["bottom"].get<int>();
                        auto top = a["top"].get<int>();
                        auto invisible = a["invisible"].get<bool>();
                        RECT r;
                        r.left = left;
                        r.right = right;
                        r.bottom = bottom;
                        r.top = top;

                        this->invisible = invisible;
                        this->videoRect = r;
                        auto hVideoWnd = GetWindow(hWebViewWnd, GW_HWNDLAST);
                        if (hVideoWnd == hWebViewWnd)
                        {
                            hVideoWnd = GetWindow(hVideoWnd, GW_HWNDPREV);
                        }
                        this->hVideoWnd = hVideoWnd;
                        if (this->invisible)
                        {
                            this->RestoreVideoWindow();
                        }
                        else
                        {
                            SetWindowPos(hVideoWnd, HWND_BOTTOM, this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                        }
                    }
                    else if (type == "invisible")
                    {
                        auto invisible = a["invisible"].get<bool>();
                        this->invisible = invisible;
                        if (this->invisible)
                        {
                            this->RestoreVideoWindow();
                        }
                        else
                        {
                            SetWindowPos(hVideoWnd, HWND_BOTTOM, this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                        }
                    }
                    else if (type == "status")
                    {
                        auto url = utf8StrToWString(a["url"].get<std::string>().c_str());
                        auto receiving = a["receiving"].get<bool>();
                        auto loading = a["loading"].get<bool>();
                        this->status.url = url;
                        this->status.receiving = receiving;
                        this->status.loading = loading;
                        this->m_pApp->StatusItemNotify(1, TVTest::STATUS_ITEM_NOTIFY_REDRAW);
                    }
                }
                return S_OK;
            }).Get(), &token);
            this->Tune();
            return S_OK;
        }).Get());
        return S_OK;
    }).Get());
}

bool CDataBroadcastingWV2::OnPluginEnable(bool fEnable)
{
    this->status = {};
    if (fEnable)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = MessageWndProc;
        wc.hInstance = g_hinstDLL;
        wc.lpszClassName = L"TVTDataBroadcastingWV2 Message Window";
        if (RegisterClassExW(&wc)) {
        }
        this->hMessageWnd = CreateWindowExW(0, L"TVTDataBroadcastingWV2 Message Window", L"TVTDataBroadcastingWV2 Message Window", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        SetWindowLongPtrW(this->hMessageWnd, GWLP_USERDATA, (LONG_PTR)this);
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL);
        Info.pMessageFunc = RemoteControlDlgProc;
        Info.pClientData = this;
        Info.hwndOwner = this->m_pApp->GetAppWindow();

        if ((HWND)this->m_pApp->ShowDialog(&Info) == nullptr)
            return false;
        ShowWindow(this->hRemoteWnd, fEnable ? SW_SHOW : SW_HIDE);

        this->hContainerWnd = FindWindowExW(FindWindowExW(FindWindowExW(this->m_pApp->GetAppWindow(), nullptr, L"TVTest Splitter", nullptr), nullptr, L"TVTest View", nullptr), nullptr, L"TVTest Video Container", nullptr);
        if (!this->hContainerWnd)
        {
            return false;
        }
        m_pApp->SetStreamCallback(0, StreamCallback, this);
        m_pApp->SetWindowMessageCallback(WindowMessageCallback, this);
        InitWebView2();
    }
    else
    {
        this->RestoreVideoWindow();
        m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback, this);
        m_pApp->SetWindowMessageCallback(nullptr, this);
        if (this->hRemoteWnd)
        {
            auto hWnd = this->hRemoteWnd;
            this->hRemoteWnd = nullptr;
            DestroyWindow(hWnd);
        }
        if (this->webViewController)
        {
            this->webView->Stop();
            this->webView.reset();
            this->webViewController->Close();
            this->webViewController.reset();
        }

        std::lock_guard<std::mutex> lock(this->packetBufferLock);
        this->packetBufferInQueue = 0;
        this->packetBufferPosition = 0;
    }
    return true;
}

void CDataBroadcastingWV2::Tune()
{
    if (this->webView && this->currentService.ServiceID)
    {
        this->status = {};
        wil::unique_cotaskmem_string source;
        if (SUCCEEDED(this->webView->get_Source(source.put())))
        {
            std::wstring baseUrl(L"https://TVTDataBroadcastingWV2.invalid/public/TVTestBML.html?serviceId=");
            baseUrl += std::to_wstring(this->currentService.ServiceID);
            if (wcscmp(source.get(), baseUrl.c_str()))
            {
                this->RestoreVideoWindow();
                // FIXME!! packetBufferは廃棄すべき
                this->webView->Navigate(baseUrl.c_str());
            }
        }
    }
}

LRESULT CALLBACK CDataBroadcastingWV2::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    return pThis->HandleEvent(Event, lParam1, lParam2, pClientData);
}

bool CDataBroadcastingWV2::OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo)
{
    std::wstring statusItem;
    if ((pInfo->Flags & TVTest::STATUS_ITEM_DRAW_FLAG_PREVIEW) == 0)
    {
        if (this->status.receiving)
        {
            statusItem += L"データ取得中...";
        }
        statusItem += this->status.url;
        if (this->status.loading)
        {
            statusItem += L"を読み込み中...";
        }
    }
    else
    {
        statusItem = L"データ取得中...";
    }
    this->m_pApp->ThemeDrawText(pInfo->pszStyle, pInfo->hdc, statusItem.c_str(),
        pInfo->DrawRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
        pInfo->Color);

    return true;
}

bool CDataBroadcastingWV2::OnFullscreenChange(bool fFullscreen)
{
    if (this->hVideoWnd && this->hMessageWnd)
    {
        if (!this->invisible)
        {
            // 無理やり動画ウィンドウを移動させている都合上リサイズ時に位置大きさが初期化されてしまうので一時的に非表示にさせる
            SetWindowPos(this->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
        // 実際に動画ウィンドウの大きさが変わるのはメッセージ処理後なのでPostMessageでやり過ごす
        PostMessageW(this->hMessageWnd, WM_APP_RESIZE, 0, 0);
    }
    return true;
}

bool CDataBroadcastingWV2::OnCommand(int ID)
{
    if (!this->webView)
    {
        return false;
    }
    switch (ID)
    {
    case IDC_KEY_D:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":20})");
        break;
    case IDC_KEY_UP:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":1})");
        break;
    case IDC_KEY_DOWN:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":2})");
        break;
    case IDC_KEY_LEFT:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":3})");
        break;
    case IDC_KEY_RIGHT:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":4})");
        break;
    case IDC_KEY_ENTER:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":18})");
        break;
    case IDC_KEY_BACK:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":19})");
        break;
    case IDC_KEY_0:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":5})");
        break;
    case IDC_KEY_1:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":6})");
        break;
    case IDC_KEY_2:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":7})");
        break;
    case IDC_KEY_3:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":8})");
        break;
    case IDC_KEY_4:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":9})");
        break;
    case IDC_KEY_5:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":10})");
        break;
    case IDC_KEY_6:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":11})");
        break;
    case IDC_KEY_7:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":12})");
        break;
    case IDC_KEY_8:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":13})");
        break;
    case IDC_KEY_9:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":14})");
        break;
    case IDC_KEY_10:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":15})");
        break;
    case IDC_KEY_11:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":16})");
        break;
    case IDC_KEY_12:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":17})");
        break;
    case IDC_KEY_BLUE:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":21})");
        break;
    case IDC_KEY_RED:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":22})");
        break;
    case IDC_KEY_GREEN:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":23})");
        break;
    case IDC_KEY_YELLOW:
        this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":24})");
        break;
    case IDC_KEY_DEVTOOL:
        this->webView->OpenDevToolsWindow();
        break;
    case IDC_KEY_RELOAD:
        this->webView->Reload();
        break;
    }
    return true;
}

INT_PTR CALLBACK CDataBroadcastingWV2::RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        pThis->hRemoteWnd = hDlg;
        return 1;
    }
    case WM_COMMAND:
    {
        CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
        pThis->OnCommand(LOWORD(wParam));
        return 1;
    }
    case WM_CLOSE:
    {
        DestroyWindow(hDlg);
        return 1;
    }
    case WM_DESTROY:
    {
        return 1;
    }
    }
    return 0;
}

TVTest::CTVTestPlugin* CreatePluginClass()
{
    return new CDataBroadcastingWV2;
}
