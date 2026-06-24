#define DUCKDB_EXTENSION_MAIN

#include "read_pdf_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// File I/O helpers
//===--------------------------------------------------------------------===//

// Read a file through DuckDB's FileSystem into a heap buffer.
// All access controls (enable_external_access, allowed_directories) are enforced here.
static std::vector<char> ReadFileBytes(FileSystem &fs, const std::string &path) {
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	int64_t raw_size = fs.GetFileSize(*handle);
	if (raw_size < 0) {
		throw IOException("read_pdf: could not determine size of '%s'", path);
	}
	auto size = static_cast<size_t>(raw_size);
	std::vector<char> buf(size);
	if (size > 0) {
		fs.Read(*handle, buf.data(), raw_size, static_cast<idx_t>(0));
	}
	return buf;
}

//===--------------------------------------------------------------------===//
// Column name helpers
//===--------------------------------------------------------------------===//

// Replace control characters and null bytes with '_', cap at 128 chars.
static std::string SanitizeColumnName(const std::string &raw) {
	std::string result;
	result.reserve(raw.size());
	for (unsigned char c : raw) {
		result += (c < 0x20 || c == 0x7f) ? '_' : static_cast<char>(c);
	}
	if (result.size() > 128) {
		result.resize(128);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// CollectFields – AcroForm field tree walk
//===--------------------------------------------------------------------===//

static constexpr int kMaxFieldDepth = 50;

// Recursively collect (full_field_name, value) pairs from an AcroForm field node.
// visited tracks indirect-object IDs to break reference cycles in malformed PDFs.
// add() returns false when the object was already seen (cycle) or it is a direct
// object (obj==0), which cannot participate in reference cycles.
static void CollectFields(QPDFObjectHandle field, const std::string &prefix,
                          std::vector<std::pair<std::string, std::string>> &out, QPDFObjGen::set &visited, int depth) {
	if (depth > kMaxFieldDepth)
		return;
	if (!visited.add(field.getObjGen()))
		return; // cycle detected
	if (!field.isDictionary())
		return;

	std::string name = prefix;
	if (field.hasKey("/T")) {
		auto t = field.getKey("/T");
		if (t.isString()) {
			auto part = t.getUTF8Value();
			name = prefix.empty() ? part : prefix + "." + part;
		}
	}

	if (field.hasKey("/Kids")) {
		auto kids = field.getKey("/Kids");
		if (kids.isArray()) {
			bool recursed = false;
			for (int i = 0; i < kids.getArrayNItems(); i++) {
				auto kid = kids.getArrayItem(i);
				if (kid.isDictionary() && kid.hasKey("/T")) {
					CollectFields(kid, name, out, visited, depth + 1);
					recursed = true;
				}
			}
			if (recursed)
				return;
		}
	}

	if (!field.hasKey("/FT") || name.empty())
		return;

	std::string value;
	if (field.hasKey("/V")) {
		auto v = field.getKey("/V");
		if (v.isString())
			value = v.getUTF8Value();
		else if (v.isName())
			value = v.getName();
	}
	out.emplace_back(name, value);
}

// Parse all AcroForm fields from a PDF loaded into `buf`.
// `buf` must stay alive for the lifetime of the returned vector.
// `path` is used as the qpdf description (appears in error messages).
static std::vector<std::pair<std::string, std::string>>
ReadFormFieldsFromMemory(const std::vector<char> &buf, const std::string &path, const std::string &password) {
	QPDF pdf;
	pdf.setSuppressWarnings(true);
	const char *pw = password.empty() ? nullptr : password.c_str();
	pdf.processMemoryFile(path.c_str(), buf.data(), buf.size(), pw);

	std::vector<std::pair<std::string, std::string>> out;
	auto root = pdf.getRoot();
	if (!root.hasKey("/AcroForm"))
		return out;
	auto acroform = root.getKey("/AcroForm");
	if (!acroform.hasKey("/Fields"))
		return out;

	auto fields = acroform.getKey("/Fields");
	QPDFObjGen::set visited;
	for (int i = 0; i < fields.getArrayNItems(); i++) {
		CollectFields(fields.getArrayItem(i), "", out, visited, 0);
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//

struct PdfFormBindData : public TableFunctionData {
	std::vector<std::string> paths;
	std::vector<std::string> column_names;    // sanitized + deduplicated, used as output column names
	std::vector<std::string> raw_field_names; // original /T values, used as lookup keys in each file
	std::string password;
	bool ignore_errors = false;
};

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> PdfFormBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<PdfFormBindData>();
	auto input_path = StringValue::Get(input.inputs[0]);
	auto &fs = FileSystem::GetFileSystem(context);

	// --- named parameters ---
	auto pw_it = input.named_parameters.find("password");
	bool has_explicit_pw = (pw_it != input.named_parameters.end() && !pw_it->second.IsNull());
	std::string explicit_pw = has_explicit_pw ? StringValue::Get(pw_it->second) : "";

	auto ie_it = input.named_parameters.find("ignore_errors");
	if (ie_it != input.named_parameters.end() && !ie_it->second.IsNull()) {
		bind_data->ignore_errors = BooleanValue::Get(ie_it->second);
	}

	// --- path expansion ---
	if (!FileSystem::HasGlob(input_path) && fs.DirectoryExists(input_path)) {
		// Directory: only collect *.pdf
		auto matches = fs.GlobFiles(fs.JoinPath(input_path, "*.pdf"));
		for (auto &f : matches)
			bind_data->paths.push_back(f.path);
		std::sort(bind_data->paths.begin(), bind_data->paths.end());
	} else if (!FileSystem::HasGlob(input_path)) {
		// Single explicit file path: accept regardless of extension
		bind_data->paths.push_back(input_path);
	} else {
		// Glob pattern: only keep .pdf matches
		auto matches = fs.GlobFiles(input_path);
		for (auto &f : matches) {
			if (f.path.size() >= 4 && f.path.compare(f.path.size() - 4, 4, ".pdf") == 0) {
				bind_data->paths.push_back(f.path);
			}
		}
		std::sort(bind_data->paths.begin(), bind_data->paths.end());
	}

	if (bind_data->paths.empty()) {
		throw BinderException("read_pdf: no PDF files found at '%s'", input_path);
	}

	// --- resolve password: explicit param > matching secret > empty string ---
	if (has_explicit_pw) {
		bind_data->password = explicit_pw;
	} else {
		// Use the Secret Manager to find a "pdf"-type secret scoped to the first file.
		// Usage: CREATE TEMPORARY SECRET (TYPE pdf, password 'hunter2', SCOPE '/my/pdfs/');
		// Prefer TEMPORARY secrets — persistent secrets are written to disk unencrypted.
		try {
			KeyValueSecretReader reader(context, "pdf", bind_data->paths[0]);
			std::string pw_from_secret;
			if (reader.TryGetSecretKey("password", pw_from_secret)) {
				bind_data->password = pw_from_secret;
			}
		} catch (...) {
			// No matching secret found; proceed with empty password.
		}
	}

	// --- derive schema from first parseable file ---
	// Schema is derived from the first file only (cheap and predictable).
	// TODO: future enhancement: union schema across all files for heterogeneous form sets.
	std::vector<std::pair<std::string, std::string>> first_fields;
	size_t schema_idx = 0;
	for (; schema_idx < bind_data->paths.size(); schema_idx++) {
		const auto &first_path = bind_data->paths[schema_idx];
		try {
			auto buf = ReadFileBytes(fs, first_path);
			first_fields = ReadFormFieldsFromMemory(buf, first_path, bind_data->password);
			if (!first_fields.empty()) {
				break; // found a usable schema source
			}
			// File parsed but had no AcroForm fields.
			if (!bind_data->ignore_errors) {
				throw BinderException("read_pdf: '%s' contains no AcroForm fields", first_path);
			}
		} catch (BinderException &) {
			throw; // propagate our own binder errors
		} catch (std::exception &e) {
			if (!bind_data->ignore_errors) {
				throw BinderException("read_pdf: failed to read '%s': %s", first_path, e.what());
			}
			// ignore_errors=true: skip this file and try the next one
		}
	}

	if (first_fields.empty()) {
		throw BinderException("read_pdf: no readable PDF with AcroForm fields found at '%s'", input_path);
	}

	// Drop files that failed before the schema-providing file; scan will skip them anyway
	// via ignore_errors, but removing them keeps the path list clean.
	if (schema_idx > 0) {
		bind_data->paths.erase(bind_data->paths.begin(), bind_data->paths.begin() + static_cast<ptrdiff_t>(schema_idx));
	}

	// --- build output columns with sanitization and deduplication ---
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("source_file");

	std::set<std::string> seen_names;
	seen_names.insert("source_file");

	for (auto &kv : first_fields) {
		std::string col = SanitizeColumnName(kv.first);
		if (col.empty())
			col = "field";

		// Disambiguate collisions (including collision with "source_file").
		if (seen_names.count(col)) {
			int suffix = 1;
			std::string candidate;
			do {
				candidate = col + "_" + std::to_string(suffix++);
			} while (seen_names.count(candidate));
			col = candidate;
		}
		seen_names.insert(col);

		bind_data->column_names.push_back(col);
		bind_data->raw_field_names.push_back(kv.first); // original name for per-file lookup

		return_types.push_back(LogicalType::VARCHAR);
		names.push_back(col);
	}

	return std::move(bind_data);
}

//===--------------------------------------------------------------------===//
// Global state
//===--------------------------------------------------------------------===//

struct PdfFormGlobalState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> PdfFormInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<PdfFormGlobalState>();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//

static void PdfFormScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<PdfFormBindData>();
	auto &gstate = data.global_state->Cast<PdfFormGlobalState>();
	auto &fs = FileSystem::GetFileSystem(context);

	idx_t row = 0;
	while (row < STANDARD_VECTOR_SIZE && gstate.file_idx < bind_data.paths.size()) {
		const auto &path = bind_data.paths[gstate.file_idx++];

		std::map<std::string, std::string> values;
		try {
			auto buf = ReadFileBytes(fs, path);
			for (auto &kv : ReadFormFieldsFromMemory(buf, path, bind_data.password)) {
				values[kv.first] = kv.second;
			}
		} catch (std::exception &e) {
			if (!bind_data.ignore_errors) {
				throw IOException("read_pdf: failed to read '%s': %s", path, e.what());
			}
			// ignore_errors=true: skip this file silently and move on
			continue;
		}

		output.SetValue(0, row, Value(path)); 
		for (idx_t col = 0; col < bind_data.column_names.size(); col++) {
			auto it = values.find(bind_data.raw_field_names[col]);
			output.SetValue(col + 1, row, (it == values.end() || it->second.empty()) ? Value() : Value(it->second));
		}
		row++;
	}
	output.SetCardinality(row);
}

//===--------------------------------------------------------------------===//
// Register
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	// Register the "pdf" secret type so users can do:
	//   CREATE TEMPORARY SECRET (TYPE pdf, password 'hunter2', SCOPE '/path/to/pdfs/');
	// TEMPORARY is strongly recommended — persistent secrets are stored on disk unencrypted.
	SecretType pdf_secret_type;
	pdf_secret_type.name = "pdf";
	pdf_secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	pdf_secret_type.default_provider = "config";
	loader.RegisterSecretType(pdf_secret_type);

	CreateSecretFunction pdf_secret_fn;
	pdf_secret_fn.secret_type = "pdf";
	pdf_secret_fn.provider = "config";
	pdf_secret_fn.named_parameters["password"] = LogicalType::VARCHAR;
	pdf_secret_fn.function = [](ClientContext &, CreateSecretInput &input) -> unique_ptr<BaseSecret> {
		auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
		secret->TrySetValue("password", input);
		secret->redact_keys.insert("password");
		return std::move(secret);
	};
	loader.RegisterFunction(pdf_secret_fn);

	// Table function
	TableFunction read_pdf("read_pdf", {LogicalType::VARCHAR}, PdfFormScan, PdfFormBind, PdfFormInitGlobal);
	read_pdf.named_parameters["password"] = LogicalType::VARCHAR;
	read_pdf.named_parameters["ignore_errors"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(read_pdf);
}

void ReadPdfExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ReadPdfExtension::Name() {
	return "read_pdf";
}

std::string ReadPdfExtension::Version() const {
#ifdef EXT_VERSION_READ_PDF
	return EXT_VERSION_READ_PDF;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(read_pdf, loader) {
	duckdb::LoadInternal(loader);
}
}
