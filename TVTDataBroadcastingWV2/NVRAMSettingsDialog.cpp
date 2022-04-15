#include "pch.h"
#include "NVRAMSettingsDialog.h"
#include "resource.h"
std::string wstrToUTF8String(const wchar_t* ws);
std::wstring utf8StrToWString(const char* s);

INT_PTR CALLBACK NVRAMSettingsDialog::DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    NVRAMSettingsDialog* pThis = static_cast<NVRAMSettingsDialog*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        if (pThis->webView)
        {
            SendDlgItemMessageW(hDlg, IDC_EDIT_ZIP, EM_LIMITTEXT, 7, 0);
            nlohmann::json msg{ { "type", "nvramRead" }, { "filename", "nvram://receiverinfo/zipcode" }, { "structure", "S:7B" } };
            std::ostringstream ss;
            // メッセージで通信するのは面倒なのでこうする
            ss << "window.sendMessage(" << msg << ")";
            auto ws = utf8StrToWString(ss.str().c_str());
            pThis->webView->ExecuteScript(ws.c_str(), Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [hDlg](HRESULT result, LPCWSTR resultObjectAsJson) -> HRESULT {
                if (SUCCEEDED(result))
                {
                    auto json = nlohmann::json::parse(wstrToUTF8String(resultObjectAsJson));
                    auto&& data = json["data"];
                    if (!data.is_array() || data.size() != 1)
                    {
                        return S_OK;
                    }
                    auto&& zip = data[0];
                    if (!zip.is_string())
                    {
                        return S_OK;
                    }
                    auto zipStr = zip.get<std::string>();
                    std::string filteredZip;
                    std::copy_if(zipStr.begin(), zipStr.end(), std::back_inserter(filteredZip), [](char c) { return c >= '0' && c <= '9'; });
                    SetDlgItemTextW(hDlg, IDC_EDIT_ZIP, utf8StrToWString(filteredZip.c_str()).c_str());
                    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_ZIP), TRUE);
                    SetFocus(GetDlgItem(hDlg, IDC_EDIT_ZIP));
                }
                return S_OK;
            }).Get());
        }
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK && IsWindowEnabled(GetDlgItem(hDlg, IDC_EDIT_ZIP)))
            {
                WCHAR zip[100]{};
                GetDlgItemTextW(hDlg, IDC_EDIT_ZIP, zip, 100);
                std::string buf;
                buf.reserve(8);
                WCHAR invalidChar = 0;
                // 半角数字以外の数字とかも入力できるのでいい感じにする
                for (auto i = 0; zip[i]; i++)
                {
                    auto c = zip[i];
                    if (c == L'-' || c == L'ー' || c == L'－')
                    {
                        continue;
                    }
                    if (c >= L'０' && c <= L'９')
                    {
                        c -= L'０';
                        c += L'0';
                    }
                    if (c >= L'0' && c <= L'9')
                    {
                        buf.push_back((char)c);
                    }
                    else
                    {
                        invalidChar = c;
                        break;
                    }
                }
                if (invalidChar)
                {
                    MessageBoxW(hDlg, (std::wstring(L"郵便番号に不正な文字「") + invalidChar + L"」が含まれています。").c_str(), L"NVRAMの設定", MB_ICONERROR | MB_OK);
                    return 1;
                }
                else if (buf.length() != 7 && buf.length() != 0)
                {
                    MessageBoxW(hDlg, L"郵便番号は7桁の数字または未入力である必要があります。", L"NVRAMの設定", MB_ICONERROR | MB_OK);
                    return 1;
                }
                if (pThis->webView)
                {
                    nlohmann::json msg{
                        { "type", "nvramWrite" },
                        { "filename", "nvram://receiverinfo/zipcode" },
                        { "structure", "S:7B" },
                        { "data", { buf } },
                    };
                    std::ostringstream ss;
                    ss << msg;
                    auto ws = utf8StrToWString(ss.str().c_str());
                    pThis->webView->PostWebMessageAsJson(ws.c_str());
                }
            }
            EndDialog(hDlg, LOWORD(wParam));
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
