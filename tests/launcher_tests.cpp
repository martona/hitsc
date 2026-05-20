#include "launcher_host_store.hpp"
#include "launcher_types.hpp"

#include <QByteArray>
#include <QUuid>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

#ifdef _WIN32

std::wstring to_wide(const QString& value)
{
    return std::wstring(
        reinterpret_cast<const wchar_t*>(value.utf16()),
        static_cast<std::size_t>(value.size()));
}

void delete_registry_tree(const QString& path)
{
    const std::wstring wide_path = to_wide(path);
    const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, wide_path.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        std::cerr << "WARN: failed to clean registry test key: " << result << "\n";
    }
}

#endif

void test_launcher_types()
{
    expect(
        hitsc::launcher_host_type_key(hitsc::LauncherHostType::Megarac)
            == QStringLiteral("megarac"),
        "MegaRAC type key");
    expect(
        hitsc::launcher_host_type_label(hitsc::LauncherHostType::Aten)
            == QStringLiteral("ATEN"),
        "ATEN type label");
    expect(
        hitsc::parse_launcher_host_type(QStringLiteral("pikvm")).value()
            == hitsc::LauncherHostType::Pikvm,
        "parse PiKVM type");
    expect(
        !hitsc::parse_launcher_host_type(QStringLiteral("unknown")).has_value(),
        "reject unknown host type");

    expect(
        hitsc::sanitize_host_name_to_url(QStringLiteral("Rack 01_BMC"))
            == QStringLiteral("https://rack-01-bmc"),
        "sanitize host name to URL");
    expect(
        hitsc::validate_launcher_url(QStringLiteral("https://bmc.example.com")),
        "validate hostname URL");
    expect(
        hitsc::validate_launcher_url(QStringLiteral("https://[::1]:8443")),
        "validate IPv6 URL");
    expect(
        !hitsc::validate_launcher_url(QStringLiteral("http://bmc.example.com")),
        "reject non-HTTPS URL");
    expect(
        hitsc::host_from_launcher_url(QStringLiteral("https://[::1]:8443"))
            == QStringLiteral("::1"),
        "extract IPv6 host");
}

#ifdef _WIN32

void test_credential_protector()
{
    const hitsc::CredentialProtector protector;
    const QByteArray plaintext("admin\0secret", 12);
    const QByteArray protected_data = protector.protect(plaintext);
    expect(!protected_data.isEmpty(), "DPAPI protected data is not empty");
    expect(protected_data != plaintext, "DPAPI output differs from plaintext");
    expect(protector.unprotect(protected_data) == plaintext, "DPAPI round-trip");
}

void test_host_store()
{
    const QString root = QStringLiteral("Software\\hitsc\\Tests\\")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    delete_registry_tree(root);

    try {
        hitsc::HostStore store(root);

        hitsc::SavedHost host;
        host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        host.type = hitsc::LauncherHostType::Pikvm;
        host.name = QStringLiteral("Lab PiKVM");
        host.url = QStringLiteral("https://127.0.0.1");
        host.credentials = hitsc::LauncherCredentials{
            QStringLiteral("admin"),
            QStringLiteral("secret"),
        };

        store.save_host(host);
        const QList<hitsc::SavedHost> loaded = store.load_hosts();
        expect(loaded.size() == 1, "registry store loads one host");
        if (!loaded.empty()) {
            expect(loaded.front().id == host.id, "registry store preserves id");
            expect(loaded.front().type == host.type, "registry store preserves type");
            expect(loaded.front().name == host.name, "registry store preserves name");
            expect(loaded.front().url == host.url, "registry store preserves URL");
            expect(loaded.front().credentials.has_value(), "registry store loads credentials");
            if (loaded.front().credentials) {
                expect(
                    loaded.front().credentials->username == QStringLiteral("admin"),
                    "registry store preserves username");
                expect(
                    loaded.front().credentials->password == QStringLiteral("secret"),
                    "registry store preserves password");
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: registry store round-trip threw: " << ex.what() << "\n";
        ++failures;
    }

    delete_registry_tree(root);
}

#endif

} // namespace

int main()
{
    test_launcher_types();

#ifdef _WIN32
    test_credential_protector();
    test_host_store();
#endif

    if (failures != 0) {
        std::cerr << failures << " launcher test(s) failed.\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
