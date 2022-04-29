#include "pch.h"
#include "InputDialog.h"
#include "resource.h"

InputDialog::InputDialog(std::wstring characterType, std::optional<std::wstring> allowedCharacters, int maxLength, std::wstring value, std::function<void(std::unique_ptr<WCHAR[]> callback)> callback) : characterType(std::move(characterType)), allowedCharacters(std::move(allowedCharacters)), maxLength(maxLength), value(std::move(value)), callback(std::move(callback))
{

}

INT_PTR CALLBACK InputDialog::DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    InputDialog* pThis = static_cast<InputDialog*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        pThis->hDlg = hDlg;
        SetFocus(GetDlgItem(hDlg, IDC_INPUT));
        SetDlgItemTextW(hDlg, IDC_INPUT, pThis->value.c_str());
        SendDlgItemMessageW(hDlg, IDC_INPUT, EM_LIMITTEXT, pThis->maxLength, 0);
        break;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK)
            {
                auto text = new WCHAR[pThis->maxLength + 1];
                text[0] = 0;
                GetDlgItemTextW(hDlg, IDC_INPUT, text, pThis->maxLength + 1);
                pThis->callback(std::unique_ptr<WCHAR[]>(text));
            }
            else
            {
                pThis->callback({});
            }
            EndDialog(hDlg, wParam);
        }
        break;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    case WM_DESTROY:
    {
        pThis->hDlg = nullptr;
        return 1;
    }
    }
    return 0;
}

InputDialog::~InputDialog()
{
    DestroyWindow(this->hDlg);
}
