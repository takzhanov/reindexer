#include "reindexer_c.h"

#include <stdlib.h>
#include <string.h>
#include <locale>
#include <mutex>

#include "core/reindexer.h"
#include "core/selectfunc/selectfuncparser.h"
#include "core/transactionimpl.h"
#include "debug/allocdebug.h"
#include "resultserializer.h"
#include "tools/logger.h"
#include "tools/stringstools.h"

using namespace reindexer;
using std::move;
const int kQueryResultsPoolSize = 1024;
const int kMaxConcurentQueries = 65534;

static Error err_not_init(-1, "Reindexer db has not initialized");
static Error err_too_many_queries(errLogic, "Too many paralell queries");

static reindexer_error error2c(const Error& err_) {
	reindexer_error err;
	err.code = err_.code();
	err.what = err_.what().length() ? strdup(err_.what().c_str()) : nullptr;
	return err;
}

static reindexer_ret ret2c(const Error& err_, const reindexer_resbuffer& out) {
	reindexer_ret ret;
	ret.err_code = err_.code();
	if (ret.err_code) {
		ret.out.results_ptr = 0;
		ret.out.data = uintptr_t(err_.what().length() ? strdup(err_.what().c_str()) : nullptr);
	} else {
		ret.out = out;
	}
	return ret;
}

static string str2c(reindexer_string gs) { return string(reinterpret_cast<const char*>(gs.p), gs.n); }
static string_view str2cv(reindexer_string gs) { return string_view(reinterpret_cast<const char*>(gs.p), gs.n); }

struct QueryResultsWrapper : QueryResults {
	WrResultSerializer ser;
};
struct TransactionWrapper {
	TransactionWrapper(Transaction&& tr) : tr_(move(tr)) {}
	WrResultSerializer ser_;
	Transaction tr_;
};

static std::mutex res_pool_lck;
static h_vector<std::unique_ptr<QueryResultsWrapper>, 2> res_pool;
static int alloced_res_count;

void put_results_to_pool(QueryResultsWrapper* res) {
	res->Clear();
	res->ser.Reset();
	std::unique_lock<std::mutex> lck(res_pool_lck);
	alloced_res_count--;
	if (res_pool.size() < kQueryResultsPoolSize)
		res_pool.push_back(std::unique_ptr<QueryResultsWrapper>(res));
	else
		delete res;
}

QueryResultsWrapper* new_results() {
	std::unique_lock<std::mutex> lck(res_pool_lck);
	if (alloced_res_count > kMaxConcurentQueries) {
		return nullptr;
	}
	alloced_res_count++;
	if (res_pool.empty()) {
		return new QueryResultsWrapper;
	} else {
		auto res = res_pool.back().release();
		res_pool.pop_back();
		return res;
	}
}

static void results2c(QueryResultsWrapper* result, struct reindexer_resbuffer* out, int with_items = 0, int32_t* pt_versions = nullptr,
					  int pt_versions_count = 0) {
	int flags = with_items ? kResultsJson : (kResultsPtrs | kResultsWithItemID);

	flags |= (pt_versions && with_items == 0) ? kResultsWithPayloadTypes : 0;

	result->ser.SetOpts({flags, span<int32_t>(pt_versions, pt_versions_count), 0, INT_MAX});

	result->ser.PutResults(result);

	out->len = result->ser.Len();
	out->data = uintptr_t(result->ser.Buf());
	out->results_ptr = uintptr_t(result);
}

uintptr_t init_reindexer() {
	Reindexer* db = new Reindexer();
	setvbuf(stdout, 0, _IONBF, 0);
	setvbuf(stderr, 0, _IONBF, 0);
	setlocale(LC_CTYPE, "");
	setlocale(LC_NUMERIC, "C");
	return reinterpret_cast<uintptr_t>(db);
}

void destroy_reindexer(uintptr_t rx) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	delete db;
	db = nullptr;
}

reindexer_error reindexer_ping(uintptr_t rx) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(db ? Error(errOK) : err_not_init);
}

Item procces_packed_item(Reindexer* db, string_view ns, int mode, int state_token, reindexer_buffer data, const vector<string>& precepts,
						 int format, Error& err) {
	Item item = db->NewItem(ns);

	if (item.Status().ok()) {
		switch (format) {
			case FormatJson:
				err = item.FromJSON(string_view(reinterpret_cast<const char*>(data.data), data.len), 0, mode == ModeDelete);
				break;
			case FormatCJson:
				if (item.GetStateToken() != state_token)
					err = Error(errStateInvalidated, "stateToken mismatch:  %08X, need %08X. Can't process item", state_token,
								item.GetStateToken());
				else
					err = item.FromCJSON(string_view(reinterpret_cast<const char*>(data.data), data.len), mode == ModeDelete);
				break;
			default:
				err = Error(-1, "Invalid source item format %d", format);
		}
		if (err.ok()) {
			item.SetPrecepts(precepts);
		}
	} else {
		err = item.Status();
	}

	return item;
}

reindexer_error reindexer_modify_item_packed_tx(uintptr_t rx, uintptr_t tr, reindexer_buffer args, reindexer_buffer data) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	TransactionWrapper* trw = reinterpret_cast<TransactionWrapper*>(tr);

	Serializer ser(args.data, args.len);
	int format = ser.GetVarUint();
	int mode = ser.GetVarUint();
	int state_token = ser.GetVarUint();
	unsigned preceptsCount = ser.GetVarUint();
	vector<string> precepts;
	while (preceptsCount--) {
		precepts.push_back(ser.GetVString().ToString());
	}
	Error err = err_not_init;
	auto item = procces_packed_item(db, trw->tr_.GetName(), mode, state_token, data, precepts, format, err);

	if (err.ok()) {
		trw->tr_.Modify(std::move(item), ItemModifyMode(mode));
	}

	return error2c(err);
}

reindexer_ret reindexer_modify_item_packed(uintptr_t rx, reindexer_buffer args, reindexer_buffer data) {
	Serializer ser(args.data, args.len);
	string_view ns = ser.GetVString();
	int format = ser.GetVarUint();
	int mode = ser.GetVarUint();
	int state_token = ser.GetVarUint();
	unsigned preceptsCount = ser.GetVarUint();
	vector<string> precepts;
	while (preceptsCount--) {
		precepts.push_back(ser.GetVString().ToString());
	}

	reindexer_resbuffer out = {0, 0, 0};
	Error err = err_not_init;
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	if (db) {
		auto item = procces_packed_item(db, ns, mode, state_token, data, precepts, format, err);

		if (err.ok()) {
			switch (mode) {
				case ModeUpsert:
					err = db->Upsert(ns, item);
					break;
				case ModeInsert:
					err = db->Insert(ns, item);
					break;
				case ModeUpdate:
					err = db->Update(ns, item);
					break;
				case ModeDelete:
					err = db->Delete(ns, item);
					break;
			}
		}

		if (err.ok()) {
			QueryResultsWrapper* res = new_results();
			res->AddItem(item);
			int32_t ptVers = -1;
			bool tmUpdated = item.IsTagsUpdated();
			results2c(res, &out, 0, tmUpdated ? &ptVers : nullptr, tmUpdated ? 1 : 0);
		}
	}

	return ret2c(err, out);
}

reindexer_tx_ret reindexer_start_transaction(uintptr_t rx, reindexer_string nsName) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	reindexer_tx_ret ret;
	if (!db) {
		ret.err = error2c(err_not_init);
		return ret;
	}
	Transaction tr = db->NewTransaction(str2cv(nsName));
	TransactionWrapper* trw = new TransactionWrapper(move(tr));
	ret.tx_id = reinterpret_cast<uintptr_t>(trw);
	ret.err = error2c(0);
	return ret;
}

reindexer_error reindexer_rollback_transaction(uintptr_t rx, uintptr_t tr) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	TransactionWrapper* trw = reinterpret_cast<TransactionWrapper*>(tr);
	auto err = db->RollBackTransaction(trw->tr_);
	delete trw;
	return error2c(err);
}

reindexer_ret reindexer_commit_transaction(uintptr_t rx, uintptr_t tr) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	TransactionWrapper* trw = reinterpret_cast<TransactionWrapper*>(tr);
	auto err = db->CommitTransaction(trw->tr_);

	reindexer_resbuffer out = {0, 0, 0};
	auto trAccessor = static_cast<TransactionAccessor*>(&trw->tr_);

	bool tmUpdated = false;

	if (err.ok()) {
		QueryResultsWrapper* res = new_results();
		for (auto& step : trAccessor->GetSteps()) {
			res->AddItem(step.item_);
			if (!tmUpdated) tmUpdated = step.item_.IsTagsUpdated();
		}
		int32_t ptVers = -1;
		results2c(res, &out, 0, tmUpdated ? &ptVers : nullptr, tmUpdated ? 1 : 0);
	}

	delete trw;
	return ret2c(err, out);
}

reindexer_error reindexer_open_namespace(uintptr_t rx, reindexer_string nsName, StorageOpts opts) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->OpenNamespace(str2cv(nsName), opts));
}

reindexer_error reindexer_drop_namespace(uintptr_t rx, reindexer_string nsName) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->DropNamespace(str2cv(nsName)));
}

reindexer_error reindexer_close_namespace(uintptr_t rx, reindexer_string nsName) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->CloseNamespace(str2cv(nsName)));
}

reindexer_error reindexer_add_index(uintptr_t rx, reindexer_string nsName, reindexer_string indexDefJson) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	string json = str2c(indexDefJson);
	IndexDef indexDef;

	try {
		indexDef.FromJSON(&json[0]);
	} catch (const Error& err) {
		return error2c(err);
	}

	return error2c(!db ? err_not_init : db->AddIndex(str2cv(nsName), indexDef));
}

reindexer_error reindexer_update_index(uintptr_t rx, reindexer_string nsName, reindexer_string indexDefJson) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	string json = str2c(indexDefJson);
	IndexDef indexDef;

	try {
		indexDef.FromJSON(&json[0]);
	} catch (const Error& err) {
		return error2c(err);
	}

	return error2c(!db ? err_not_init : db->UpdateIndex(str2cv(nsName), indexDef));
}

reindexer_error reindexer_drop_index(uintptr_t rx, reindexer_string nsName, reindexer_string index) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->DropIndex(str2cv(nsName), IndexDef(str2c(index))));
}

reindexer_error reindexer_enable_storage(uintptr_t rx, reindexer_string path) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->EnableStorage(str2c(path)));
}

reindexer_error reindexer_init_system_namespaces(uintptr_t rx) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->InitSystemNamespaces());
}

reindexer_ret reindexer_select(uintptr_t rx, reindexer_string query, int with_items, int32_t* pt_versions, int pt_versions_count) {
	reindexer_resbuffer out = {0, 0, 0};
	Error res = err_not_init;
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	if (db) {
		QueryResultsWrapper* result = new_results();
		if (!result) return ret2c(err_too_many_queries, out);
		res = db->Select(str2cv(query), *result);
		if (res.ok())
			results2c(result, &out, with_items, pt_versions, pt_versions_count);
		else
			put_results_to_pool(result);
	}
	return ret2c(res, out);
}

reindexer_ret reindexer_select_query(uintptr_t rx, struct reindexer_buffer in, int with_items, int32_t* pt_versions,
									 int pt_versions_count) {
	Error res = err_not_init;
	reindexer_resbuffer out = {0, 0, 0};
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	if (db) {
		res = Error(errOK);
		Serializer ser(in.data, in.len);

		Query q;
		q.Deserialize(ser);
		while (!ser.Eof()) {
			Query q1;
			q1.joinType = JoinType(ser.GetVarUint());
			q1.Deserialize(ser);
			q1.debugLevel = q.debugLevel;
			if (q1.joinType == JoinType::Merge) {
				q.mergeQueries_.emplace_back(std::move(q1));
			} else {
				q.joinQueries_.emplace_back(std::move(q1));
			}
		}

		QueryResultsWrapper* result = new_results();
		if (!result) return ret2c(err_too_many_queries, out);
		res = db->Select(q, *result);
		if (q.debugLevel >= LogError && res.code() != errOK) logPrintf(LogError, "Query error %s", res.what());
		if (res.ok())
			results2c(result, &out, with_items, pt_versions, pt_versions_count);
		else
			put_results_to_pool(result);
	}
	return ret2c(res, out);
}

reindexer_ret reindexer_delete_query(uintptr_t rx, reindexer_buffer in) {
	reindexer_resbuffer out{0, 0, 0};
	Error res = err_not_init;
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	if (db) {
		res = Error(errOK);
		Serializer ser(in.data, in.len);

		Query q;
		q.Deserialize(ser);
		QueryResultsWrapper* result = new_results();
		if (!result) return ret2c(err_too_many_queries, out);
		res = db->Delete(q, *result);
		if (q.debugLevel >= LogError && res.code() != errOK) logPrintf(LogError, "Query error %s", res.what());
		if (res.ok())
			results2c(result, &out);
		else
			put_results_to_pool(result);
	}
	return ret2c(res, out);
}

reindexer_error reindexer_put_meta(uintptr_t rx, reindexer_string ns, reindexer_string key, reindexer_string data) {
	Error res = err_not_init;
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	if (db) {
		res = db->PutMeta(str2c(ns), str2c(key), str2c(data));
	}
	return error2c(res);
}

reindexer_ret reindexer_get_meta(uintptr_t rx, reindexer_string ns, reindexer_string key) {
	reindexer_resbuffer out{0, 0, 0};
	Error res = err_not_init;
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	if (db) {
		QueryResultsWrapper* results = new_results();
		if (!results) return ret2c(err_too_many_queries, out);

		string data;
		res = db->GetMeta(str2c(ns), str2c(key), data);
		results->ser.Write(data);
		out.len = results->ser.Len();
		out.data = uintptr_t(results->ser.Buf());
		out.results_ptr = uintptr_t(results);
	}
	return ret2c(res, out);
}

reindexer_error reindexer_commit(uintptr_t rx, reindexer_string nsName) {
	Reindexer* db = reinterpret_cast<Reindexer*>(rx);
	return error2c(!db ? err_not_init : db->Commit(str2cv(nsName)));
}

void reindexer_enable_logger(void (*logWriter)(int, char*)) { logInstallWriter(logWriter); }

void reindexer_disable_logger() { logInstallWriter(nullptr); }

reindexer_error reindexer_free_buffer(reindexer_resbuffer in) {
	put_results_to_pool(reinterpret_cast<QueryResultsWrapper*>(in.results_ptr));
	return error2c(Error(errOK));
}

reindexer_error reindexer_free_buffers(reindexer_resbuffer* in, int count) {
	for (int i = 0; i < count; i++) {
		reindexer_free_buffer(in[i]);
	}
	return error2c(Error(errOK));
}
