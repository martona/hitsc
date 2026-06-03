#include "launcher_host_store.hpp"

#include "errors.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "windows_registry.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#else
#include <QSettings>
#endif

namespace hitsc {
namespace {

constexpr DWORD kRegistrySchemaVersion = 1;

QByteArray serialize_credentials(const LauncherCredentials& credentials)
{
    QJsonObject root;
    root.insert(QStringLiteral("username"), credentials.username);
    root.insert(QStringLiteral("password"), credentials.password);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<LauncherCredentials> deserialize_credentials(const QByteArray& bytes)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }

    const QJsonObject root = document.object();
    LauncherCredentials credentials;
    credentials.username = root.value(QStringLiteral("username")).toString();
    credentials.password = root.value(QStringLiteral("password")).toString();
    if (credentials.empty()) {
        return std::nullopt;
    }
    return credentials;
}

#ifdef _WIN32

void write_schema_version(HKEY root)
{
    DWORD version = kRegistrySchemaVersion;
    const LONG result = RegSetValueExW(
        root,
        L"SchemaVersion",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&version),
        sizeof(version));
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to write registry schema version", result);
    }
}

std::optional<SavedHost> read_host_from_key(
    HKEY key,
    const QString& id,
    const CredentialProtector& protector)
{
    const auto type_value = read_string_value(key, L"Type");
    const auto url_value = read_string_value(key, L"Url");
    if (!type_value || !url_value) {
        return std::nullopt;
    }

    const std::optional<LauncherHostType> type = parse_launcher_host_type(*type_value);
    if (!type) {
        return std::nullopt;
    }

    if (!validate_launcher_url(*url_value)) {
        return std::nullopt;
    }

    SavedHost host;
    host.id = id;
    host.type = *type;
    host.url = *url_value;

    const auto protected_credentials = read_binary_value(key, L"Credentials");
    if (protected_credentials && !protected_credentials->isEmpty()) {
        try {
            host.credentials =
                deserialize_credentials(protector.unprotect(*protected_credentials));
        } catch (const std::exception&) {
            host.credentials = std::nullopt;
        }
    }

    return host;
}

#endif

} // namespace

HostStore::HostStore(QString root_path)
    : root_path_(std::move(root_path))
{
}

const QString& HostStore::root_path() const
{
    return root_path_;
}

#ifdef _WIN32

QByteArray CredentialProtector::protect(const QByteArray& plaintext) const
{
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.constData()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};
    if (!CryptProtectData(
            &input,
            L"hitsc saved host credentials",
            nullptr,
            nullptr,
            nullptr,
            0,
            &output)) {
        throw std::runtime_error("failed to protect saved host credentials");
    }

    QByteArray result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

QByteArray CredentialProtector::unprotect(const QByteArray& ciphertext) const
{
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(ciphertext.constData()));
    input.cbData = static_cast<DWORD>(ciphertext.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        throw std::runtime_error("failed to unprotect saved host credentials");
    }

    QByteArray result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

QList<SavedHost> HostStore::load_hosts() const
{
    const RegistryKey root = create_key(HKEY_CURRENT_USER, root_path_);
    write_schema_version(root.get());
    const RegistryKey hosts = create_key(root.get(), QStringLiteral("Hosts"));

    QList<SavedHost> loaded;
    DWORD index = 0;
    while (true) {
        std::wstring key_name(256, L'\0');
        DWORD key_name_length = static_cast<DWORD>(key_name.size());
        const LONG result = RegEnumKeyExW(
            hosts.get(),
            index,
            key_name.data(),
            &key_name_length,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            throw_windows_error("failed to enumerate saved hosts", result);
        }

        key_name.resize(key_name_length);
        const QString id = from_wide(key_name);
        const auto host_key = open_key(hosts.get(), id, KEY_READ);
        if (host_key) {
            const auto host = read_host_from_key(host_key->get(), id, protector_);
            if (host) {
                loaded.append(*host);
            }
        }
        ++index;
    }

    std::sort(loaded.begin(), loaded.end(), saved_host_hostname_less);
    return loaded;
}

void HostStore::save_host(const SavedHost& host) const
{
    if (host.id.trimmed().isEmpty()) {
        throw UserError("saved host id is empty");
    }

    const RegistryKey root = create_key(HKEY_CURRENT_USER, root_path_);
    write_schema_version(root.get());
    const RegistryKey hosts = create_key(root.get(), QStringLiteral("Hosts"));
    const RegistryKey host_key = create_key(hosts.get(), host.id);

    write_string_value(host_key.get(), L"Type", launcher_host_type_key(host.type));
    write_string_value(host_key.get(), L"Url", host.url);

    if (host.credentials && !host.credentials->empty()) {
        write_binary_value(
            host_key.get(),
            L"Credentials",
            protector_.protect(serialize_credentials(*host.credentials)));
    } else {
        const LONG result = RegDeleteValueW(host_key.get(), L"Credentials");
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
            throw_windows_error("failed to delete saved host credentials", result);
        }
    }
}

void HostStore::delete_host(const QString& id) const
{
    const QString trimmed_id = id.trimmed();
    if (trimmed_id.isEmpty()) {
        throw UserError("saved host id is empty");
    }

    const RegistryKey root = create_key(HKEY_CURRENT_USER, root_path_);
    write_schema_version(root.get());
    const RegistryKey hosts = create_key(root.get(), QStringLiteral("Hosts"));

    const std::wstring wide_id = to_wide(trimmed_id);
    const LONG result = RegDeleteTreeW(hosts.get(), wide_id.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        throw_windows_error("failed to delete saved host", result);
    }
}

void HostStore::save_last_connected(const QString& host_id) const
{
    const RegistryKey root = create_key(HKEY_CURRENT_USER, root_path_);
    write_string_value(root.get(), L"LastConnected", host_id);
}

QString HostStore::load_last_connected() const
{
    const auto root = open_key(HKEY_CURRENT_USER, root_path_, KEY_READ);
    if (!root) {
        return {};
    }
    return read_string_value(root->get(), L"LastConnected").value_or(QString());
}

void HostStore::save_window_rect(const QString& host_id, const QRect& rect) const
{
    const QString trimmed_id = host_id.trimmed();
    if (trimmed_id.isEmpty() || rect.width() <= 0 || rect.height() <= 0) {
        return;
    }

    const auto root = open_key(HKEY_CURRENT_USER, root_path_, KEY_READ);
    if (!root) {
        return;
    }
    const auto hosts = open_key(root->get(), QStringLiteral("Hosts"), KEY_READ);
    if (!hosts) {
        return;
    }
    const auto host_key = open_key(hosts->get(), trimmed_id, KEY_WRITE);
    if (!host_key) {
        return; // host no longer exists — don't recreate it
    }

    write_int_value(host_key->get(), L"WindowX", rect.x());
    write_int_value(host_key->get(), L"WindowY", rect.y());
    write_int_value(host_key->get(), L"WindowWidth", rect.width());
    write_int_value(host_key->get(), L"WindowHeight", rect.height());
}

std::optional<QRect> HostStore::load_window_rect(const QString& host_id) const
{
    const QString trimmed_id = host_id.trimmed();
    if (trimmed_id.isEmpty()) {
        return std::nullopt;
    }

    const auto root = open_key(HKEY_CURRENT_USER, root_path_, KEY_READ);
    if (!root) {
        return std::nullopt;
    }
    const auto hosts = open_key(root->get(), QStringLiteral("Hosts"), KEY_READ);
    if (!hosts) {
        return std::nullopt;
    }
    const auto host_key = open_key(hosts->get(), trimmed_id, KEY_READ);
    if (!host_key) {
        return std::nullopt;
    }

    const auto x = read_int_value(host_key->get(), L"WindowX");
    const auto y = read_int_value(host_key->get(), L"WindowY");
    const auto width = read_int_value(host_key->get(), L"WindowWidth");
    const auto height = read_int_value(host_key->get(), L"WindowHeight");
    if (!x || !y || !width || !height) {
        return std::nullopt;
    }

    return QRect(*x, *y, *width, *height);
}

#else

QByteArray CredentialProtector::protect(const QByteArray& plaintext) const
{
    (void)plaintext;
    throw UserError("credential protection is not implemented on this platform");
}

QByteArray CredentialProtector::unprotect(const QByteArray& ciphertext) const
{
    (void)ciphertext;
    throw UserError("credential protection is not implemented on this platform");
}

QList<SavedHost> HostStore::load_hosts() const
{
    return {};
}

void HostStore::save_host(const SavedHost& host) const
{
    (void)host;
    throw UserError("saved host storage is not implemented on this platform");
}

void HostStore::delete_host(const QString& id) const
{
    (void)id;
    throw UserError("saved host storage is not implemented on this platform");
}

void HostStore::save_last_connected(const QString& host_id) const
{
    (void)host_id;
}

QString HostStore::load_last_connected() const
{
    return {};
}

void HostStore::save_window_rect(const QString& host_id, const QRect& rect) const
{
    (void)host_id;
    (void)rect;
}

std::optional<QRect> HostStore::load_window_rect(const QString& host_id) const
{
    (void)host_id;
    return std::nullopt;
}

#endif

} // namespace hitsc
