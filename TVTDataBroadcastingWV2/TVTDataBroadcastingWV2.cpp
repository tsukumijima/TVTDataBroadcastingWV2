#include "pch.h"

#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "thirdparty/TVTestPlugin.h"
#include "resource.h"
#include <queue>
#include <optional>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#include "NVRAMSettingsDialog.h"

using namespace Microsoft::WRL;

// #pragma comment(lib, "ntdll.lib")
// extern "C" ULONG DbgPrint(PCSTR Format, ...);

// メッセージウィンドウ向けメッセージ
#define WM_APP_PACKET (WM_APP + 0)
#define WM_APP_RESIZE (WM_APP + 1)

#define IDT_SHOW_EVR_WINDOW 1
#define IDT_RESIZE 2

struct Status
{
    std::wstring url;
    bool receiving = false;
    bool loading = false;
};

struct PacketQueue
{
private:
    std::mutex queueMutex;
    std::vector<BYTE> currentBlock;
    std::queue<std::vector<BYTE>> queue;
    std::atomic<bool> invalidate;
public:
    const size_t packetSize = 188;
    // PCRパケットの間隔程度で処理することが望ましい
    const size_t packetBlockSize = packetSize * 500;
    const size_t maxQueueLength = 100;

    PacketQueue()
    {
        currentBlock.reserve(packetBlockSize);
    }

    // TSスレッドでのみ呼び出せる
    bool enqueuePacket(BYTE* packet)
    {
        if (this->invalidate.exchange(false))
        {
            this->currentBlock.clear();
        }
        this->currentBlock.insert(this->currentBlock.end(), packet, packet + this->packetSize);
        if (this->currentBlock.size() >= packetBlockSize)
        {
            std::lock_guard<std::mutex> lock(this->queueMutex);
            if (this->queue.size() < this->maxQueueLength)
            {
                this->queue.push(std::move(this->currentBlock));
                this->currentBlock.reserve(this->packetBlockSize);
            }
            else
            {
                this->currentBlock.clear();
            }
            return true;
        }
        return false;
    }

    // どのスレッドからも呼び出せる
    void clear()
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        std::queue<std::vector<BYTE>>().swap(this->queue);
        this->invalidate = true;
    }

    // どのスレッドからも呼び出せる
    std::optional<std::vector<BYTE>> pop()
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        if (this->queue.empty())
        {
            return std::nullopt;
        }
        auto r = std::move(this->queue.front());
        this->queue.pop();
        return r;
    }
};

// Plugins/TVTDataBroadcastingWV2.tvtp
class CDataBroadcastingWV2 : public TVTest::CTVTestPlugin, TVTest::CTVTestEventHandler
{
    // Plugins/TVTDataBroadcastingWV2.ini
    std::wstring iniFile;
    // Plugins/TVTDataBroadcastingWV2/
    std::wstring baseDirectory;
    // Plugins/TVTDataBroadcastingWV2/resources/
    std::wstring resourceDirectory;
    // Plugins/TVTDataBroadcastingWV2/WebView2Data/
    std::wstring webView2DataDirectory;
    // Plugins/TVTDataBroadcastingWV2/WebView2/
    std::wstring webView2Directory;

    std::atomic<bool> webViewLoaded;
    PacketQueue packetQueue;

    HWND hRemoteWnd = nullptr;
    HWND hPanelWnd = nullptr;
    HWND hVideoWnd = nullptr;
    HWND hWebViewWnd = nullptr;
    HWND hViewWnd = nullptr;
    HWND hContainerWnd = nullptr;
    HWND hMessageWnd = nullptr;
    HBRUSH hbrPanelBack = nullptr;
    HFONT hPanelFont = nullptr;
    wil::com_ptr<IBasicVideo> basicVideo;
    wil::com_ptr<IBaseFilter> vmr7Renderer;
    wil::com_ptr<IBaseFilter> vmr9Renderer;
    bool invisible = false;
    RECT videoRect = {};
    RECT containerRect = {};
    TVTest::ServiceInfo currentService = {};
    TVTest::ChannelInfo currentChannel = {};
    std::unordered_set<WORD> pesPIDList;
    Status status;
    bool deferWebView = false;
    const int MAX_VOLUME = 100;
    int currentVolume = MAX_VOLUME;
    bool useTVTestVolume = false;
    virtual bool OnChannelChange();
    virtual bool OnServiceChange();
    virtual bool OnServiceUpdate();
    virtual bool OnCommand(int ID);
    virtual bool OnPluginEnable(bool fEnable);
    virtual void OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo);
    virtual void OnFilterGraphFinalize(TVTest::FilterGraphInfo* pInfo);
    virtual bool OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo);
    virtual bool OnFullscreenChange(bool fFullscreen);
    virtual bool OnPluginSettings(HWND hwndOwner);
    virtual bool OnColorChange();
    virtual bool OnPanelItemNotify(TVTest::PanelItemEventInfo* pInfo);
    virtual bool OnVolumeChange(int Volume, bool fMute);

    HWND GetFullscreenWindow();
    void RestoreVideoWindow();
    void ResizeVideoWindow();
    void Tune();
    void InitWebView2();
    bool caption = false;
    void SetCaptionState(bool enable);
    void UpdateCaptionState(bool showIndicator);
    void UpdateVolume();
    std::wstring GetIniItem(const wchar_t* key, const wchar_t* def);
    INT GetIniItem(const wchar_t* key, INT def);
    bool SetIniItem(const wchar_t* key, const wchar_t* data);
    void Disable(bool finalize);
    void EnablePanelButtons(bool enable);

    wil::com_ptr<ICoreWebView2Controller> webViewController;
    wil::com_ptr<ICoreWebView2> webView;

    static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData);
    static INT_PTR CALLBACK RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static INT_PTR CALLBACK PanelRemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
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
    pInfo->Flags = TVTest::PLUGIN_FLAG_DISABLEONSTART | TVTest::PLUGIN_FLAG_HASSETTINGS;
    pInfo->pszPluginName = L"TVTDataBroadcastingWV2";
    pInfo->pszCopyright = L"2022 otya";
#ifdef TVTDATABROADCASTINGWV2_VERSION
    pInfo->pszDescription = L"データ放送を表示" TVTDATABROADCASTINGWV2_VERSION;
#else
    pInfo->pszDescription = L"データ放送を表示";
#endif
    return true;
}

BOOL CALLBACK CDataBroadcastingWV2::StreamCallback(BYTE* pData, void* pClientData)
{
    auto pThis = (CDataBroadcastingWV2*)pClientData;
    if (!pThis->webViewLoaded)
    {
        return TRUE;
    }
    if (pThis->packetQueue.enqueuePacket(pData))
    {
        PostMessageW(pThis->hMessageWnd, WM_APP_PACKET, 0, 0);
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
        if (pThis->hContainerWnd && pThis->hMessageWnd)
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
        item.resize(size);
    }
}
INT CDataBroadcastingWV2::GetIniItem(const wchar_t* key, INT def)
{
    return GetPrivateProfileIntW(L"TVTDataBroadcastingWV2", key, def, this->iniFile.c_str());
}

bool CDataBroadcastingWV2::SetIniItem(const wchar_t* key, const wchar_t* data)
{
    return WritePrivateProfileStringW(L"TVTDataBroadcastingWV2", key, data, this->iniFile.c_str());
}

bool CDataBroadcastingWV2::Initialize()
{
    this->hbrPanelBack = CreateSolidBrush(this->m_pApp->GetColor(L"PanelBack"));
    auto filename = wil::GetModuleFileNameW<std::wstring>(g_hinstDLL);
    std::filesystem::path path(filename);
    path.replace_extension();
    baseDirectory = path;
    resourceDirectory = path / L"resources";
    webView2DataDirectory = path / L"WebView2Data";
    webView2Directory = path / L"WebView2";
    path.replace_extension(L".ini");
    this->iniFile = path;
    m_pApp->SetEventCallback(EventCallback, this);
    m_pApp->RegisterCommand(IDC_KEY_D, L"DataButton", L"dボタン");
    m_pApp->RegisterCommand(IDC_KEY_D_OR_ENABLE_PLUGIN, L"DataButtonOrEnablePlugin", L"プラグイン有効/dボタン");
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
    m_pApp->RegisterCommand(IDC_ENABLE_CAPTION, L"EnableCaption", L"字幕表示");
    m_pApp->RegisterCommand(IDC_DISABLE_CAPTION, L"DisableCaption", L"字幕非表示");
    m_pApp->RegisterCommand(IDC_TOGGLE_CAPTION, L"ToggleCaption", L"字幕表示/非表示切替");
    m_pApp->RegisterCommand(IDC_SHOW_REMOTE_CONTROL, L"ShowRemoteControl", L"リモコン表示");
    m_pApp->RegisterCommand(IDC_TASKMANAGER, L"TaskManager", L"タスクマネージャー");
    m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN));
    TVTest::PanelItemInfo panel = {};
    panel.Size = sizeof(panel);
    panel.Style = TVTest::PANEL_ITEM_STYLE_NEEDFOCUS;
    panel.pszIDText = L"TVTDataBroadcastingWV2Panel";
    panel.pszTitle = L"データ放送";
    panel.ID = 1;
    panel.hbmIcon = (HBITMAP)LoadImageW(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    m_pApp->RegisterPanelItem(&panel);
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
    if (this->GetIniItem(L"AutoEnable", 0))
    {
        m_pApp->EnablePlugin(true);
    }
    return true;
}

bool CDataBroadcastingWV2::Finalize()
{
    this->Disable(true);
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
        switch (wParam)
        {
        case IDT_SHOW_EVR_WINDOW:
        {
            SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            KillTimer(hWnd, wParam);
            break;
        }
        case IDT_RESIZE:
        {
            if (pThis->webViewController)
            {
                // FIXME: Fullscreen
                // Containerウィンドウが非表示になった場合Viewウィンドウを親にする
                if (!IsWindowVisible(pThis->hWebViewWnd))
                {
                    pThis->webViewController->put_ParentWindow(pThis->hViewWnd);
                }
                // Containerウィンドウが表示されていてViewウィンドウが親ならばContainerウィンドウを親にする
                else if (IsWindowVisible(pThis->hContainerWnd))
                {
                    HWND parent;
                    if (SUCCEEDED(pThis->webViewController->get_ParentWindow(&parent)))
                    {
                        if (parent != pThis->hContainerWnd)
                        {
                            pThis->webViewController->put_ParentWindow(pThis->hContainerWnd);
                        }
                    }
                }
                RECT rect;
                if (GetClientRect(pThis->hContainerWnd, &rect))
                {
                    if (memcmp(&pThis->containerRect, &rect, sizeof(RECT)))
                    {
                        pThis->containerRect = rect;
                        pThis->webViewController->put_Bounds(rect);
                    }
                }
                if (!pThis->invisible)
                {
                    pThis->ResizeVideoWindow();
                }
            }
            break;
        }
        }
        break;
    }
    case WM_APP_RESIZE:
    {
        if (pThis->webViewController)
        {
            if (pThis->hVideoWnd)
            {
                SetTimer(pThis->hMessageWnd, IDT_SHOW_EVR_WINDOW, 50, nullptr);
            }
            RECT rect;
            if (GetClientRect(pThis->hContainerWnd, &rect))
            {
                pThis->containerRect = rect;
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
        while (true)
        {
            auto packets = pThis->packetQueue.pop();
            if (!packets)
            {
                break;
            }
            if (!pThis->webView)
            {
                continue;
            }
            WCHAR head[] = LR"({"type":"stream","data":[)";
            WCHAR tail[] = LR"(]})";
            auto packetBlockSize = pThis->packetQueue.packetBlockSize;
            auto packetSize = pThis->packetQueue.packetSize;
            size_t size = _countof(head) - 1 + packetBlockSize * 4 /* '255,' */ + _countof(tail) + 1;
            std::unique_ptr<WCHAR[]> buf_ptr(new WCHAR[size]);
            {
                auto buf = buf_ptr.get();
                wcscpy_s(buf, size, head);
                size_t pos = 0;
                pos += wcslen(head);
                auto buffer = packets.value().data();
                for (size_t p = 0; p < packetBlockSize; p += packetSize)
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
                    for (size_t i = p; i < p + packetSize; i++)
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
            }
        }
        break;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

HWND CDataBroadcastingWV2::GetFullscreenWindow()
{
    TVTest::HostInfo info;
    std::wstring appName(L"TVTest");
    if (this->m_pApp->GetHostInfo(&info))
    {
        appName = info.pszAppName;
    }
    struct Args
    {
        std::wstring fullscreenClass;
        HWND containerWnd;
    } args = { appName + L" Fullscreen" , nullptr };
    // フルスクリーンのとき
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto args = (Args*)lParam;
        WCHAR className[100];
        if (GetClassNameW(hWnd, className, _countof(className)))
        {
            if (!wcscmp(className, args->fullscreenClass.c_str()))
            {
                args->containerWnd = hWnd;
                return false;
            }
        }
        return true;
    }, (LPARAM)&args);
    return args.containerWnd;
}

void CDataBroadcastingWV2::OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo)
{
    bool isRenderless = false;
    // VMR9
    if (SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR9", this->vmr9Renderer.put())))
    {
        auto filterConfig = this->vmr9Renderer.query<IVMRFilterConfig9>();
        VMR9Mode mode;
        if (FAILED(filterConfig->GetRenderingMode((DWORD*)&mode)) || mode != VMR9Mode_Windowless)
        {
            // VMR9 (Renderless)
            // TVTest Video Container側の大きさを変えればいいけど字幕の位置も変わってしまう
            this->vmr9Renderer = nullptr;
            this->m_pApp->AddLog(L"VMR9 (Renderless)は非推奨", TVTest::LOG_TYPE_WARNING);
            isRenderless = true;
        }
    }
    // VMR7
    else if (SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR7", this->vmr7Renderer.put())) || SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR", this->vmr7Renderer.put())))
    {
        auto filterConfig = this->vmr7Renderer.try_query<IVMRFilterConfig>();
        VMRMode mode;
        if (filterConfig && (FAILED(filterConfig->GetRenderingMode((DWORD*)&mode)) || mode != VMRMode_Windowless))
        {
            // VMR7 (Renderless)
            // TVTest Video Container側の大きさを変えればいいけど字幕の位置も変わってしまう
            this->vmr7Renderer = nullptr;
            this->m_pApp->AddLog(L"VMR7 (Renderless)は非推奨", TVTest::LOG_TYPE_WARNING);
            isRenderless = true;
        }
    }
    // システムデフォルト
    else if (SUCCEEDED(pInfo->pGraphBuilder->QueryInterface(this->basicVideo.put())))
    {
        long l, t, w, h;
        if (FAILED(this->basicVideo->GetDestinationPosition(&l, &t, &w, &h)))
        {
            // EVR
            this->basicVideo = nullptr;
        }
    }
    std::vector<HWND> childWindows;
    TVTest::HostInfo info;
    std::wstring appName(L"TVTest");
    if (this->m_pApp->GetHostInfo(&info))
    {
        appName = info.pszAppName;
    }
    auto splitterClass = appName + L" Splitter";
    auto viewClass = appName + L" View";
    auto videoContainerClass = appName + L" Video Container";
    this->hViewWnd = FindWindowExW(FindWindowExW(this->m_pApp->GetAppWindow(), nullptr, splitterClass.c_str(), nullptr), nullptr, viewClass.c_str(), nullptr);
    this->hContainerWnd = FindWindowExW(this->hViewWnd, nullptr, videoContainerClass.c_str(), nullptr);
    if (!this->hContainerWnd)
    {
        auto fullscreenWnd = this->GetFullscreenWindow();
        this->hViewWnd = FindWindowExW(FindWindowExW(fullscreenWnd, nullptr, splitterClass.c_str(), nullptr), nullptr, viewClass.c_str(), nullptr);
        this->hContainerWnd = FindWindowExW(this->hViewWnd, nullptr, videoContainerClass.c_str(), nullptr);
    }
    // まず動画ウィンドウをクラス名で検索してみる
    // 0.10
    this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"LibISDB EVR Video Window", nullptr);
    if (this->hVideoWnd == nullptr)
    {
        // 0.9
        this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"Bon DTV EVR Video Window", nullptr);
    }
    if (this->hVideoWnd == nullptr)
    {
        this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"madVR", nullptr);
    }
    // 見つからなければNotification BarでもなくPseudo OSDでもないウィンドウを動画ウィンドウとする
    if (this->hVideoWnd == nullptr)
    {
        auto notifBarClass = appName + L" Notification Bar";
        auto pseudoOSDClass = appName + L" Pseudo OSD";
        EnumChildWindows(this->hContainerWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto childWindows = (std::vector<HWND>*)lParam;
            childWindows->push_back(hWnd);
            return true;
        }, (LPARAM)&childWindows);
        for (auto it = std::rbegin(childWindows); it != std::rend(childWindows); ++it)
        {
            auto hWnd = *it;
            if (GetParent(hWnd) != this->hContainerWnd)
            {
                continue;
            }
            WCHAR className[100];
            if (GetClassNameW(hWnd, className, _countof(className)))
            {
                if (className == notifBarClass || className == pseudoOSDClass)
                {
                    continue;
                }
                if (hWnd == hWebViewWnd)
                {
                    continue;
                }
                this->hVideoWnd = hWnd;
            }
        }
    }
    if (isRenderless)
    {
        this->hVideoWnd = this->hContainerWnd;
        this->hContainerWnd = GetParent(this->hContainerWnd);
    }
    if (this->deferWebView)
    {
        this->deferWebView = false;
        this->InitWebView2();
    }
    if (this->hVideoWnd)
    {
        // madVR EVR EVR (Custom Renderer)
        SetWindowPos(this->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (this->basicVideo)
    {
        this->hVideoWnd = nullptr;
    }
    this->ResizeVideoWindow();
}

void CDataBroadcastingWV2::OnFilterGraphFinalize(TVTest::FilterGraphInfo* pInfo)
{
    this->basicVideo = nullptr;
    this->vmr7Renderer = nullptr;
    this->vmr9Renderer = nullptr;
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
    if (!this->hContainerWnd)
    {
        this->deferWebView = true;
        return;
    }
    LPCWSTR webView2Directory = nullptr;
    if (std::filesystem::is_directory(std::filesystem::path(this->webView2Directory) / L"EBWebView"))
    {
        webView2Directory = this->webView2Directory.c_str();
    }
    auto resourceDirectory = this->GetIniItem(L"ResourceDirectory", this->resourceDirectory.c_str());
    if (!std::filesystem::is_directory(resourceDirectory))
    {
        MessageBoxW(this->m_pApp->GetAppWindow(), (L"リソースディレクトリが見つかりません。\n" + resourceDirectory).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        this->m_pApp->EnablePlugin(false);
        return;
    }
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required");
    auto result = CreateCoreWebView2EnvironmentWithOptions(webView2Directory, this->webView2DataDirectory.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, resourceDirectory](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (!env)
        {
            wchar_t buf[9]{};
            _itow_s(result, buf, 16);
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
            this->m_pApp->EnablePlugin(false);
            return S_OK;
        }
        wil::unique_cotaskmem_string ver;
        if (SUCCEEDED(env->get_BrowserVersionString(ver.put())))
        {
            this->m_pApp->AddLog((std::wstring(L"WebView2 version: ") + ver.get()).c_str(), TVTest::LOG_TYPE_INFORMATION);
        }
        env->CreateCoreWebView2Controller(this->hContainerWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [env, this, resourceDirectory](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(result))
            {
                wchar_t buf[9]{};
                _itow_s(result, buf, 16);
                MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
                this->m_pApp->EnablePlugin(false);
                return S_OK;
            }
            if (controller != nullptr) {
                this->webViewController = controller;
                this->webViewController->get_CoreWebView2(this->webView.put());
            }
            auto hWebViewWnd = FindWindowExW(this->hContainerWnd, nullptr, L"Chrome_WidgetWin_0", nullptr);
            this->hWebViewWnd = hWebViewWnd;
            // 動画ウィンドウといい感じに合成させるために必要 (Windows 8以降じゃないと動かないはず)
            SetWindowLongW(hWebViewWnd, GWL_EXSTYLE, GetWindowLongW(hWebViewWnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
            SetWindowPos(hWebViewWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            // ICoreWebView2_3, ICoreWebView2Controller2: 1.0.774.44
            auto controller2 = this->webViewController.try_query<ICoreWebView2Controller2>();
            if (!controller2)
            {
                MessageBoxW(this->m_pApp->GetAppWindow(), L"WebView2のバージョンが古すぎます。", L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
            }
            else
            {
                COREWEBVIEW2_COLOR c = { };
                auto ff = controller2->put_DefaultBackgroundColor(c);
            }
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
                        auto left = a["left"].get<double>();
                        auto right = a["right"].get<double>();
                        auto top = a["top"].get<double>();
                        auto bottom = a["bottom"].get<double>();
                        auto invisible = a["invisible"].get<bool>();
                        RECT r;
                        r.left = (int)std::floor(left);
                        r.right = (int)std::ceil(right);
                        r.top = (int)std::floor(top);
                        r.bottom = (int)std::ceil(bottom);

                        this->invisible = invisible;
                        this->videoRect = r;
                        this->ResizeVideoWindow();
                    }
                    else if (type == "invisible")
                    {
                        auto invisible = a["invisible"].get<bool>();
                        this->invisible = invisible;
                        this->ResizeVideoWindow();
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
                    else if (type == "tune")
                    {
                        auto originalNetworkId = a["originalNetworkId"].get<int>();
                        auto transportStreamId = a["transportStreamId"].get<int>();
                        auto serviceId = a["serviceId"].get<int>();
                        TVTest::ChannelSelectInfo info = {};
                        info.Size = sizeof(info);
                        info.Flags = TVTest::CHANNEL_SELECT_FLAG_STRICTSERVICE;
                        // FIXME: original_network_idでない
                        info.NetworkID = originalNetworkId;
                        info.TransportStreamID = transportStreamId;
                        info.ServiceID = serviceId;
                        info.Channel = -1;
                        info.Space = -1;
                        if (!this->m_pApp->SelectChannel(&info))
                        {
                            this->m_pApp->AddLog((L"選局に失敗しました。(original_network_id=" + std::to_wstring(originalNetworkId) + L",transport_stream_id=" + std::to_wstring(transportStreamId) + L",service_id=" + std::to_wstring(serviceId) + L")").c_str(), TVTest::LOG_TYPE_ERROR);
                            if (this->webView)
                            {
                                this->webView->Reload();
                            }
                        }
                    }
                }
                return S_OK;
            }).Get(), &token);

            this->webView->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                this->UpdateCaptionState(false);
                this->UpdateVolume();
                this->webViewLoaded = true;
                return S_OK;
            }).Get(), &token);
            this->Tune();
            return S_OK;
        }).Get());
        return S_OK;
    }).Get());
    if (FAILED(result))
    {
        wchar_t buf[9]{};
        _itow_s(result, buf, 16);

        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(CreateCoreWebView2EnvironmentWithOptions)\nWebView2がインストールされていない可能性があります。\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        }
        else
        {
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(CreateCoreWebView2EnvironmentWithOptions)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        }
        this->m_pApp->EnablePlugin(false);
    }
}

void CDataBroadcastingWV2::ResizeVideoWindow()
{
    if (this->invisible)
    {
        this->RestoreVideoWindow();
    }
    else
    {
        if (this->videoRect.right - this->videoRect.left && this->videoRect.bottom - this->videoRect.top)
        {
            if (this->vmr9Renderer)
            {
                auto vmr9WindowlessControl = this->vmr9Renderer.query<IVMRWindowlessControl9>();
                vmr9WindowlessControl->SetVideoPosition(nullptr, &this->videoRect);
            }
            else if (this->vmr7Renderer)
            {
                auto vmr7WindowlessControl = this->vmr7Renderer.query<IVMRWindowlessControl>();
                vmr7WindowlessControl->SetVideoPosition(nullptr, &this->videoRect);
            }
            else if (this->basicVideo)
            {
                this->basicVideo->SetDestinationPosition(this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top);
            }
            else
            {
                RECT rect;
                if (GetClientRect(this->hVideoWnd, &rect))
                {
                    if (memcmp(&rect, &this->videoRect, sizeof(rect)))
                    {
                        SetWindowPos(this->hVideoWnd, HWND_BOTTOM, this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                    }
                }
            }
        }
    }
}

void CDataBroadcastingWV2::Disable(bool finalize)
{
    this->RestoreVideoWindow();
    m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback, this);
    if (!finalize)
    {
        m_pApp->SetWindowMessageCallback(nullptr, this);
    }
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
    this->webViewLoaded = false;

    this->packetQueue.clear();

    if (this->hMessageWnd)
    {
        auto hWnd = this->hMessageWnd;
        DestroyWindow(hWnd);
        this->hMessageWnd = nullptr;
    }

    if (finalize)
    {
        auto hWnd = this->hPanelWnd;
        this->hPanelWnd = nullptr;
        DestroyWindow(hWnd);
        DeleteObject(this->hbrPanelBack);
        this->hbrPanelBack = nullptr;
        if (this->hPanelFont)
        {
            DeleteObject(this->hPanelFont);
            this->hPanelFont = nullptr;
        }
    }
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

        if (!this->GetIniItem(L"DisableRemoteControl", 0))
        {
            this->OnCommand(IDC_SHOW_REMOTE_CONTROL);
        }
        this->useTVTestVolume = this->GetIniItem(L"UseTVTestVolume", true);
        m_pApp->SetStreamCallback(0, StreamCallback, this);
        m_pApp->SetWindowMessageCallback(WindowMessageCallback, this);
        InitWebView2();
        SetTimer(this->hMessageWnd, IDT_RESIZE, 1000, nullptr);
        this->EnablePanelButtons(true);
    }
    else
    {
        this->EnablePanelButtons(false);
        this->Disable(false);
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
            std::wstring baseUrl(L"https://TVTDataBroadcastingWV2.invalid/TVTDataBroadcastingWV2.html?serviceId=");
            baseUrl += std::to_wstring(this->currentService.ServiceID);
            baseUrl += L"&networkId=";
            baseUrl += std::to_wstring(this->currentChannel.NetworkID);
            if (_wcsicmp(source.get(), baseUrl.c_str()))
            {
                this->RestoreVideoWindow();
                this->packetQueue.clear();
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
    if (this->hContainerWnd && this->hMessageWnd)
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

void CDataBroadcastingWV2::UpdateCaptionState(bool showIndicator)
{
    nlohmann::json msg{ { "type", "caption" }, { "enable", this->caption }, { "showIndicator", showIndicator } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

void CDataBroadcastingWV2::UpdateVolume()
{
    nlohmann::json msg{ { "type", "volume" }, { "value", this->useTVTestVolume ? this->currentVolume / (double)MAX_VOLUME : 1.0 } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

void CDataBroadcastingWV2::SetCaptionState(bool enable)
{
    this->caption = enable;
    if (this->hRemoteWnd)
    {
        SendDlgItemMessageW(this->hRemoteWnd, IDC_TOGGLE_CAPTION, BM_SETCHECK, this->caption ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (this->hPanelWnd)
    {
        SendDlgItemMessageW(this->hPanelWnd, IDC_TOGGLE_CAPTION, BM_SETCHECK, this->caption ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    this->UpdateCaptionState(true);
}

bool CDataBroadcastingWV2::OnCommand(int ID)
{
    if (ID == IDC_SHOW_REMOTE_CONTROL)
    {
        if (!this->hRemoteWnd)
        {
            TVTest::ShowDialogInfo Info;

            Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
            Info.hinst = g_hinstDLL;
            Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL);
            Info.pMessageFunc = RemoteControlDlgProc;
            Info.pClientData = this;
            Info.hwndOwner = this->m_pApp->GetAppWindow();

            if ((HWND)this->m_pApp->ShowDialog(&Info) == nullptr)
                return false;
            ShowWindow(this->hRemoteWnd, SW_SHOW);
        }
        return true;
    }
    if (ID == IDC_KEY_SETTINGS)
    {
        if (this->m_pApp->GetFullscreen())
        {
            return this->OnPluginSettings(this->GetFullscreenWindow());
        }
        return this->OnPluginSettings(this->m_pApp->GetAppWindow());
    }
    if (!this->webView)
    {
        if (ID == IDC_KEY_D_OR_ENABLE_PLUGIN)
        {
            if (!this->m_pApp->IsPluginEnabled())
            {
                this->m_pApp->EnablePlugin(true);
                return true;
            }
        }
        return false;
    }
    switch (ID)
    {
    case IDC_KEY_D_OR_ENABLE_PLUGIN:
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
    case IDC_TOGGLE_CAPTION:
        this->SetCaptionState(!this->caption);
        break;
    case IDC_ENABLE_CAPTION:
        this->SetCaptionState(true);
        break;
    case IDC_DISABLE_CAPTION:
        this->SetCaptionState(false);
        break;
    case IDC_TASKMANAGER:
    {
        auto webView6 = this->webView.try_query<ICoreWebView2_6>();
        if (webView6)
        {
            webView6->OpenTaskManagerWindow();
        }
        break;
    }
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
        if (pThis->caption)
        {
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_CAPTION, BM_SETCHECK, BST_CHECKED, 0);
        }
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDC_TOGGLE_CAPTION)
        {
            if (SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED)
            {
                pThis->OnCommand(IDC_ENABLE_CAPTION);
            }
            else
            {
                pThis->OnCommand(IDC_DISABLE_CAPTION);
            }
        }
        else
        {
            pThis->OnCommand(LOWORD(wParam));
        }
        return 1;
    }
    case WM_CLOSE:
    {
        DestroyWindow(hDlg);
        return 1;
    }
    case WM_DESTROY:
    {
        pThis->hRemoteWnd = nullptr;
        return 1;
    }
    }
    return 0;
}

void CDataBroadcastingWV2::EnablePanelButtons(bool enable)
{
    if (!this->hPanelWnd)
    {
        return;
    }
    EnumChildWindows(this->hPanelWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto id = GetDlgCtrlID(hWnd);
        if (id != IDC_KEY_D_OR_ENABLE_PLUGIN && id != IDC_KEY_SETTINGS)
        {
            EnableWindow(hWnd, (bool)lParam);
        }
        return true;
    }, (LPARAM)enable);
}

INT_PTR CALLBACK CDataBroadcastingWV2::PanelRemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        pThis->hPanelWnd = hDlg;
        if (pThis->caption)
        {
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_CAPTION, BM_SETCHECK, BST_CHECKED, 0);
        }
        return 1;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
        return (INT_PTR)pThis->hbrPanelBack;
    case WM_DESTROY:
    {
        pThis->hPanelWnd = nullptr;
        return 1;
    }
    }
    return RemoteControlDlgProc(hDlg, uMsg, wParam, lParam, pClientData);
}

INT_PTR CALLBACK CDataBroadcastingWV2::SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        if (pThis->GetIniItem(L"DisableRemoteControl", 0))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_DISABLE_REMOTECON, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"AutoEnable", 0))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_AUTOENABLE, BM_SETCHECK, BST_CHECKED, 0);
        }
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK)
            {
                auto disableRemoteControl = SendDlgItemMessageW(hDlg, IDC_CHECK_DISABLE_REMOTECON, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"DisableRemoteControl", disableRemoteControl ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                auto autoEnable = SendDlgItemMessageW(hDlg, IDC_CHECK_AUTOENABLE, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"AutoEnable", autoEnable ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
            }
            EndDialog(hDlg, LOWORD(wParam));
        }
        else if (LOWORD(wParam) == IDC_BUTTON_NVRAM_SETTING)
        {
            if (!pThis->webView)
            {
                MessageBoxW(hDlg, L"プラグインが有効の間のみ郵便番号・保存領域の設定を行えます。", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
            }
            else
            {
                TVTest::ShowDialogInfo Info;

                Info.Flags = 0;
                Info.hinst = g_hinstDLL;
                Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTING_NVRAM);
                Info.pMessageFunc = NVRAMSettingsDialog::DlgProc;
                std::unique_ptr< NVRAMSettingsDialog> dialog(new NVRAMSettingsDialog(pThis->webView));
                Info.pClientData = dialog.get();
                Info.hwndOwner = hDlg;

                pThis->m_pApp->ShowDialog(&Info);
            }
        }
        return 1;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    case WM_DESTROY:
    {
        return 1;
    }
    }
    return 0;
}

bool CDataBroadcastingWV2::OnPluginSettings(HWND hwndOwner)
{
    TVTest::ShowDialogInfo Info;

    Info.Flags = 0;
    Info.hinst = g_hinstDLL;
    Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTING);
    Info.pMessageFunc = SettingsDlgProc;
    Info.pClientData = this;
    Info.hwndOwner = hwndOwner;

    auto result = this->m_pApp->ShowDialog(&Info);
    return result == IDOK;
}

bool CDataBroadcastingWV2::OnColorChange()
{
    if (this->hbrPanelBack)
    {
        DeleteObject(this->hbrPanelBack);
        this->hbrPanelBack = CreateSolidBrush(this->m_pApp->GetColor(L"PanelBack"));
    }
    return true;
}

bool CDataBroadcastingWV2::OnPanelItemNotify(TVTest::PanelItemEventInfo* pInfo)
{
    switch (pInfo->Event)
    {
    case TVTest::PANEL_ITEM_EVENT_CREATE:
    {
        auto createEventInfo = CONTAINING_RECORD(pInfo, TVTest::PanelItemCreateEventInfo, EventInfo);
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL_PANEL);
        Info.pMessageFunc = PanelRemoteControlDlgProc;
        Info.pClientData = this;
        Info.hwndOwner = createEventInfo->hwndParent;
        auto hWnd = (HWND)this->m_pApp->ShowDialog(&Info);
        ShowWindow(hWnd, SW_SHOW);
        createEventInfo->hwndItem = hWnd;
        this->EnablePanelButtons(this->m_pApp->IsPluginEnabled());
    }
    [[fallthrough]];
    case TVTest::PANEL_ITEM_EVENT_FONTCHANGED:
    {
        // フォントが大きすぎるとはみ出してしまうのでフォントの大きさに合わせてボタンの大きさを変更すべきではある
        // TVTestがやっているように全部自前で描画して処理するのは大変なのでダイアログで妥協
        if (this->hPanelFont)
        {
            DeleteObject(this->hPanelFont);
        }
        LOGFONTW lf;
        m_pApp->GetFont(L"PanelFont", &lf);
        this->hPanelFont = CreateFontIndirectW(&lf);
        SendMessageW(this->hPanelWnd, WM_SETFONT, (WPARAM)this->hPanelFont, true);
        EnumChildWindows(this->hPanelWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto hFont = (HFONT)lParam;
            SendMessageW(hWnd, WM_SETFONT, (WPARAM)hFont, 0);
            return true;
        }, (LPARAM)this->hPanelFont);
        break;
    }
    }
    return true;
}

bool CDataBroadcastingWV2::OnVolumeChange(int Volume, bool fMute)
{
    this->currentVolume = fMute ? 0 : Volume;
    this->UpdateVolume();
    return true;
}

TVTest::CTVTestPlugin* CreatePluginClass()
{
    return new CDataBroadcastingWV2;
}
