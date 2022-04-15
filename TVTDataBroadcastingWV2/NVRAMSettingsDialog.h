#pragma once
class NVRAMSettingsDialog
{
    wil::com_ptr<ICoreWebView2> webView;
public:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    NVRAMSettingsDialog(wil::com_ptr<ICoreWebView2> webView) : webView(webView) {}
};
