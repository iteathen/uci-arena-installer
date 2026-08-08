#include "arena/ports/asset_search_index.hpp"

#define NOMINMAX
#include <windows.h>

#include <msdasc.h>
#include <oledb.h>
#include <oledberr.h>
#include <searchapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>

namespace arena::ports {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct RowBuffer {
    DBSTATUS status{};
    DBLENGTH length{};
    wchar_t value[32768]{};
};

class ComApartment final {
  public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT result_{};
};

std::wstring ascii_widen(const std::string_view value) {
    return {value.begin(), value.end()};
}

std::wstring filename_glob_to_like(const std::string_view glob) {
    std::wstring result;
    result.reserve(glob.size() + 8);
    for (const char character : glob) {
        if (character == '*') {
            result.push_back(L'%');
        } else if (character == '?') {
            result.push_back(L'_');
        } else if (character == '_') {
            result.append(L"[_]");
        } else if (character == '%') {
            result.append(L"[%]");
        } else {
            result.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(character)));
        }
    }
    return result;
}

std::wstring rule_predicate(const IndexedAssetRule& rule) {
    std::wstring predicate = L"(";
    bool first = true;
    auto append = [&](const std::wstring& condition) {
        if (!first) {
            predicate.append(L" OR ");
        }
        predicate.append(condition);
        first = false;
    };
    for (const auto& extension : rule.file_extensions) {
        append(L"System.FileExtension = '" + ascii_widen(extension) + L"'");
    }
    for (const auto& glob : rule.filename_globs) {
        if (glob.find_first_of("*?") == std::string::npos) {
            append(L"System.ItemNameDisplay = '" + ascii_widen(glob) + L"'");
        } else {
            append(L"System.ItemNameDisplay LIKE '" + filename_glob_to_like(glob) + L"'");
        }
    }
    predicate.push_back(L')');
    return predicate;
}

std::wstring query_sql(const std::size_t limit, const IndexedAssetRule& rule) {
    return L"SELECT TOP " + std::to_wstring(limit) +
           L" System.ItemPathDisplay FROM SystemIndex WHERE " +
           rule_predicate(rule);
}

HRESULT set_command_timeout(ICommandText* command, const std::chrono::milliseconds remaining) {
    ComPtr<ICommandProperties> properties;
    HRESULT result = command->QueryInterface(IID_PPV_ARGS(properties.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    DBPROP property{};
    property.dwPropertyID = DBPROP_COMMANDTIMEOUT;
    property.dwOptions = DBPROPOPTIONS_OPTIONAL;
    property.colid = DB_NULLID;
    VariantInit(&property.vValue);
    property.vValue.vt = VT_I4;
    property.vValue.lVal = std::max<LONG>(
        1, static_cast<LONG>((remaining.count() + 999) / 1000));
    DBPROPSET property_set{&property, 1, DBPROPSET_ROWSET};
    result = properties->SetProperties(1, &property_set);
    VariantClear(&property.vValue);
    return result;
}

HRESULT execute_query(IDBCreateCommand* command_factory, const std::wstring& sql,
                      const IndexedAssetKind kind, const std::size_t limit,
                      const Clock::time_point deadline,
                      std::vector<IndexedAssetCandidate>& candidates,
                      bool& truncated) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return DB_E_CANCELED;
    }
    ComPtr<ICommandText> command;
    HRESULT result = command_factory->CreateCommand(
        nullptr, IID_ICommandText,
        reinterpret_cast<IUnknown**>(command.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    result = command->SetCommandText(DBGUID_DEFAULT, sql.c_str());
    if (FAILED(result)) {
        return result;
    }
    (void)set_command_timeout(
        command.Get(),
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));

    ComPtr<IRowset> rowset;
    DBROWCOUNT affected{};
    result = command->Execute(
        nullptr, IID_IRowset, nullptr, &affected,
        reinterpret_cast<IUnknown**>(rowset.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IAccessor> accessor;
    result = rowset.As(&accessor);
    if (FAILED(result)) {
        return result;
    }
    DBBINDING binding{};
    binding.iOrdinal = 1;
    binding.obStatus = offsetof(RowBuffer, status);
    binding.obLength = offsetof(RowBuffer, length);
    binding.obValue = offsetof(RowBuffer, value);
    binding.dwPart = DBPART_STATUS | DBPART_LENGTH | DBPART_VALUE;
    binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
    binding.eParamIO = DBPARAMIO_NOTPARAM;
    binding.cbMaxLen = sizeof(RowBuffer::value);
    binding.wType = DBTYPE_WSTR;
    DBBINDSTATUS binding_status{};
    HACCESSOR row_accessor{};
    result = accessor->CreateAccessor(
        DBACCESSOR_ROWDATA, 1, &binding, sizeof(RowBuffer),
        &row_accessor, &binding_status);
    if (FAILED(result) || binding_status != DBBINDSTATUS_OK) {
        return FAILED(result) ? result : DB_E_ERRORSOCCURRED;
    }

    std::size_t accepted{};
    for (;;) {
        if (Clock::now() >= deadline) {
            truncated = true;
            result = DB_E_CANCELED;
            break;
        }
        DBCOUNTITEM row_count{};
        HROW* rows{};
        result = rowset->GetNextRows(DB_NULL_HCHAPTER, 0, 16, &row_count, &rows);
        if (result == DB_S_ENDOFROWSET || row_count == 0) {
            result = S_OK;
            if (rows != nullptr) {
                CoTaskMemFree(rows);
            }
            break;
        }
        if (FAILED(result)) {
            if (rows != nullptr) {
                CoTaskMemFree(rows);
            }
            break;
        }
        for (DBCOUNTITEM index = 0; index < row_count; ++index) {
            RowBuffer row{};
            if (SUCCEEDED(rowset->GetData(rows[index], row_accessor, &row)) &&
                row.status == DBSTATUS_S_OK && row.length > sizeof(wchar_t) &&
                row.value[0] != L'\0') {
                candidates.push_back(
                    {kind, std::filesystem::path(std::wstring(row.value))});
                ++accepted;
            }
        }
        rowset->ReleaseRows(row_count, rows, nullptr, nullptr, nullptr);
        CoTaskMemFree(rows);
        if (accepted >= limit) {
            truncated = true;
            result = S_OK;
            break;
        }
    }
    accessor->ReleaseAccessor(row_accessor, nullptr);
    return result;
}

class WindowsSearchIndex final : public AssetSearchIndex {
  public:
    [[nodiscard]] IndexedAssetSearchResult
    search(const IndexedAssetQuery& query) override {
        IndexedAssetSearchResult result;
        result.adapter = "windows_search_system_index";
        result.maximum_results = query.maximum_results;
        const auto started = Clock::now();
        const auto finish = [&](const IndexedSearchState state,
                                const std::string_view detail) {
            result.state = state;
            result.detail_code = detail;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started);
            return result;
        };
        if (!valid_indexed_asset_query(query)) {
            return finish(IndexedSearchState::failed, "invalid_search_budget");
        }
        ComApartment apartment;
        if (!apartment.usable()) {
            return finish(IndexedSearchState::unavailable, "com_unavailable");
        }

        ComPtr<ISearchManager> manager;
        HRESULT status = CoCreateInstance(
            __uuidof(CSearchManager), nullptr, CLSCTX_LOCAL_SERVER,
            IID_PPV_ARGS(manager.GetAddressOf()));
        if (FAILED(status)) {
            return finish(IndexedSearchState::unavailable,
                          "windows_search_unavailable");
        }
        ComPtr<ISearchCatalogManager> catalog;
        status = manager->GetCatalog(L"SystemIndex", catalog.GetAddressOf());
        if (FAILED(status)) {
            return finish(IndexedSearchState::unavailable,
                          "system_index_unavailable");
        }
        ComPtr<ISearchQueryHelper> query_helper;
        status = catalog->GetQueryHelper(query_helper.GetAddressOf());
        if (FAILED(status)) {
            return finish(IndexedSearchState::failed,
                          "query_helper_unavailable");
        }
        wchar_t* connection_string{};
        status = query_helper->get_ConnectionString(&connection_string);
        if (FAILED(status) || connection_string == nullptr) {
            return finish(IndexedSearchState::failed,
                          "index_connection_unavailable");
        }

        ComPtr<IDataInitialize> data_initialize;
        status = CoCreateInstance(
            CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(data_initialize.GetAddressOf()));
        if (FAILED(status)) {
            CoTaskMemFree(connection_string);
            return finish(IndexedSearchState::unavailable,
                          "oledb_unavailable");
        }
        ComPtr<IUnknown> data_source_unknown;
        status = data_initialize->GetDataSource(
            nullptr, CLSCTX_INPROC_SERVER, connection_string, IID_IDBInitialize,
            data_source_unknown.GetAddressOf());
        CoTaskMemFree(connection_string);
        if (FAILED(status)) {
            return finish(IndexedSearchState::unavailable,
                          "index_connection_failed");
        }
        ComPtr<IDBInitialize> data_source;
        status = data_source_unknown.As(&data_source);
        if (FAILED(status) || FAILED(data_source->Initialize())) {
            return finish(IndexedSearchState::unavailable,
                          "index_initialization_failed");
        }
        ComPtr<IDBCreateSession> session_factory;
        status = data_source.As(&session_factory);
        ComPtr<IUnknown> session_unknown;
        if (SUCCEEDED(status)) {
            status = session_factory->CreateSession(
                nullptr, IID_IDBCreateCommand, session_unknown.GetAddressOf());
        }
        ComPtr<IDBCreateCommand> command_factory;
        if (SUCCEEDED(status)) {
            status = session_unknown.As(&command_factory);
        }
        if (FAILED(status)) {
            data_source->Uninitialize();
            return finish(IndexedSearchState::failed,
                          "index_session_failed");
        }

        const auto deadline = started + query.timeout;
        const std::size_t per_kind = std::max<std::size_t>(
            1, query.maximum_results / query.rules.size());
        for (const auto& rule : query.rules) {
            status = execute_query(
                command_factory.Get(), query_sql(per_kind, rule), rule.kind,
                per_kind, deadline, result.candidates, result.truncated);
            if (status == DB_E_CANCELED || Clock::now() >= deadline) {
                data_source->Uninitialize();
                return finish(IndexedSearchState::timed_out,
                              "search_budget_exhausted");
            }
            if (FAILED(status)) {
                data_source->Uninitialize();
                return finish(IndexedSearchState::failed,
                              "index_query_failed");
            }
        }
        data_source->Uninitialize();
        std::ranges::sort(result.candidates, {}, [](const IndexedAssetCandidate& candidate) {
            return candidate.path.native();
        });
        result.candidates.erase(
            std::unique(result.candidates.begin(), result.candidates.end(),
                        [](const IndexedAssetCandidate& left,
                           const IndexedAssetCandidate& right) {
                            return left.kind == right.kind && left.path == right.path;
                        }),
            result.candidates.end());
        if (result.candidates.size() > query.maximum_results) {
            result.candidates.resize(query.maximum_results);
            result.truncated = true;
        }
        return finish(IndexedSearchState::complete,
                      result.truncated ? "result_limit_reached" : "complete");
    }
};

} // namespace

std::unique_ptr<AssetSearchIndex> make_platform_asset_search_index() {
    return std::make_unique<WindowsSearchIndex>();
}

} // namespace arena::ports
