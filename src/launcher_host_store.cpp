#include "launcher_host_store.hpp"

#include "errors.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
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

std::wstring to_wide(const QString& value)
{
    return std::wstring(
        reinterpret_cast<const wchar_t*>(value.utf16()),
        static_cast<std::size_t>(value.size()));
}

QString from_wide(const std::wstring& value)
{
    return QString::fromWCharArray(value.c_str(), static_cast<int>(value.size()));
}

[[noreturn]] void throw_windows_error(const char* context, LONG error)
{
    throw std::system_error(
        static_cast<int>(error),
        std::system_category(),
        context);
}

class RegistryKey {
public:
    RegistryKey() = default;
    explicit RegistryKey(HKEY key)
        : key_(key)
    {
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    RegistryKey(RegistryKey&& other) noexcept
        : key_(other.key_)
    {
        other.key_ = nullptr;
    }

    RegistryKey& operator=(RegistryKey&& other) noexcept
    {
        if (this != &other) {
            close();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    ~RegistryKey()
    {
        close();
    }

    HKEY get() const
    {
        return key_;
    }

private:
    void close()
    {
        if (key_ != nullptr) {
            RegCloseKey(key_);
            key_ = nullptr;
        }
    }

    HKEY key_ = nullptr;
};

RegistryKey create_key(HKEY parent, const QString& path)
{
    HKEY key = nullptr;
    const std::wstring wide_path = to_wide(path);
    const LONG result = RegCreateKeyExW(
        parent,
        wide_path.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        nullptr,
        &key,
        nullptr);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to create registry key", result);
    }
    return RegistryKey(key);
}

std::optional<RegistryKey> open_key(HKEY parent, const QString& path, REGSAM access)
{
    HKEY key = nullptr;
    const std::wstring wide_path = to_wide(path);
    const LONG result = RegOpenKeyExW(parent, wide_path.c_str(), 0, access, &key);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to open registry key", result);
    }
    return RegistryKey(key);
}

void write_string_value(HKEY key, const wchar_t* name, const QString& value)
{
    const std::wstring wide_value = to_wide(value);
    const auto byte_count =
        static_cast<DWORD>((wide_value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(wide_value.c_str()),
        byte_count);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to write registry string value", result);
    }
}

std::optional<QString> read_string_value(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD byte_count = 0;
    LONG result =
        RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &byte_count);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry string value size", result);
    }

    std::wstring buffer(byte_count / sizeof(wchar_t), L'\0');
    result = RegGetValueW(
        key,
        nullptr,
        name,
        RRF_RT_REG_SZ,
        &type,
        buffer.data(),
        &byte_count);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry string value", result);
    }

    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return from_wide(buffer);
}

void write_binary_value(HKEY key, const wchar_t* name, const QByteArray& value)
{
    const LONG result = RegSetValueExW(
        key,
        name,
        0,
        REG_BINARY,
        reinterpret_cast<const BYTE*>(value.constData()),
        static_cast<DWORD>(value.size()));
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to write registry binary value", result);
    }
}

std::optional<QByteArray> read_binary_value(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD byte_count = 0;
    LONG result =
        RegGetValueW(key, nullptr, name, RRF_RT_REG_BINARY, &type, nullptr, &byte_count);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry binary value size", result);
    }

    QByteArray buffer;
    buffer.resize(static_cast<qsizetype>(byte_count));
    result = RegGetValueW(
        key,
        nullptr,
        name,
        RRF_RT_REG_BINARY,
        &type,
        buffer.data(),
        &byte_count);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry binary value", result);
    }
    buffer.resize(static_cast<qsizetype>(byte_count));
    return buffer;
}

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
    const auto name_value = read_string_value(key, L"Name");
    const auto url_value = read_string_value(key, L"Url");
    if (!type_value || !name_value || !url_value) {
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
    host.name = *name_value;
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

    std::sort(loaded.begin(), loaded.end(), [](const SavedHost& left, const SavedHost& right) {
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });
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
    write_string_value(host_key.get(), L"Name", host.name);
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

#endif

} // namespace hitsc
