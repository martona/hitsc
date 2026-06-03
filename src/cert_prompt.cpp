#include "cert_prompt.hpp"

#ifdef _WIN32

// Bind to v6 of the common controls so TaskDialogIndirect is available and
// themed. Localised here so the rest of the build needs no manifest juggling.
#if defined(_MSC_VER)
#pragma comment( \
    linker, \
    "/manifestdependency:\"type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <windows.h>

#include <commctrl.h>

#include <string>

namespace hitsc {
namespace {

std::wstring widen(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), length);
    return wide;
}

std::wstring build_content(
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint)
{
    std::wstring content = L"hitsc could not confirm the identity of ";
    content += widen(host);
    content += L".";

    if (!info.reason.empty()) {
        content += L"\n\n";
        content += widen(info.reason);
    }

    content += L"\n";
    if (!info.subject.empty()) {
        content += L"\nName:  " + widen(info.subject);
    }
    if (!info.issuer.empty()) {
        content += L"\nIssuer:  " + widen(info.issuer);
    }
    if (!info.valid_from.empty() || !info.valid_to.empty()) {
        content += L"\nValid:  " + widen(info.valid_from) + L"  to  " + widen(info.valid_to);
    }
    content += L"\nSHA-256:  " + widen(info.fingerprint);

    if (changed && previous_fingerprint) {
        content += L"\n\nPreviously pinned:\n" + widen(*previous_fingerprint);
    }

    return content;
}

} // namespace

CertPromptResult show_cert_prompt(
    void* native_parent,
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint,
    bool allow_pin)
{
    const HWND parent = reinterpret_cast<HWND>(native_parent);

    const std::wstring title = L"hitsc";
    const std::wstring instruction =
        changed ? L"This host's certificate has changed" : L"Untrusted certificate";
    const std::wstring content = build_content(host, info, changed, previous_fingerprint);
    const std::wstring verification =
        changed ? L"Replace the pinned certificate with this one"
                : L"Pin this certificate and remember it for this host";

    constexpr int kConnectId = 1001;
    TASKDIALOG_BUTTON connect_button{kConnectId, L"Connect"};

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = parent;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = changed ? TD_ERROR_ICON : TD_WARNING_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.pButtons = &connect_button;
    config.cButtons = 1;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.nDefaultButton = changed ? IDCANCEL : kConnectId;
    if (allow_pin) {
        config.pszVerificationText = verification.c_str();
    }

    int pressed = 0;
    BOOL checked = FALSE;
    const HRESULT result = TaskDialogIndirect(&config, &pressed, nullptr, &checked);
    if (FAILED(result)) {
        // Common controls v6 unavailable — degrade to a plain prompt. The
        // remember checkbox is lost; the user can still connect once or bail.
        const std::wstring fallback_title = title + L" - certificate";
        const UINT flags = MB_OKCANCEL | (changed ? MB_ICONERROR | MB_DEFBUTTON2 : MB_ICONWARNING);
        const int answer = MessageBoxW(parent, content.c_str(), fallback_title.c_str(), flags);
        CertPromptResult fallback;
        fallback.proceed = answer == IDOK;
        fallback.pin = false;
        return fallback;
    }

    CertPromptResult choice;
    choice.proceed = pressed == kConnectId;
    choice.pin = allow_pin && choice.proceed && checked != FALSE;
    return choice;
}

} // namespace hitsc

#else // !_WIN32

namespace hitsc {

CertPromptResult show_cert_prompt(
    void*,
    const std::string&,
    const CertPromptInfo&,
    bool,
    const std::optional<std::string>&,
    bool)
{
    return {};
}

} // namespace hitsc

#endif // _WIN32
