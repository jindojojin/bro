#include <string>

#include "Info.h"
#include "InfoTimer.h"
#include "FileID.h"
#include "Reporter.h"
#include "Val.h"
#include "Type.h"

#include "Action.h"
#include "Extract.h"
#include "Hash.h"
#include "DataEvent.h"

using namespace file_analysis;

// keep in order w/ declared enum values in file_analysis.bif
static ActionInstantiator action_factory[] = {
    Extract::Instantiate,
    MD5::Instantiate,
    SHA1::Instantiate,
    SHA256::Instantiate,
    DataEvent::Instantiate,
};

static TableVal* empty_conn_id_set()
	{
	TypeList* set_index = new TypeList(conn_id);
	set_index->Append(conn_id->Ref());
	return new TableVal(new SetType(set_index, 0));
	}

static StringVal* get_conn_uid_val(Connection* conn)
	{
	char tmp[20];
	if ( ! conn->GetUID() )
		conn->SetUID(calculate_unique_id());
    return new StringVal(uitoa_n(conn->GetUID(), tmp, sizeof(tmp), 62));
	}

static RecordVal* get_conn_id_val(const Connection* conn)
	{
	RecordVal* v = new RecordVal(conn_id);
	v->Assign(0, new AddrVal(conn->OrigAddr()));
	v->Assign(1, new PortVal(ntohs(conn->OrigPort()), conn->ConnTransport()));
	v->Assign(2, new AddrVal(conn->RespAddr()));
	v->Assign(3, new PortVal(ntohs(conn->RespPort()), conn->ConnTransport()));
	return v;
	}

int Info::file_id_idx = -1;
int Info::parent_file_id_idx = -1;
int Info::protocol_idx = -1;
int Info::conn_uids_idx = -1;
int Info::conn_ids_idx = -1;
int Info::seen_bytes_idx = -1;
int Info::total_bytes_idx = -1;
int Info::missing_bytes_idx = -1;
int Info::overflow_bytes_idx = -1;
int Info::timeout_interval_idx = -1;
int Info::actions_idx = -1;

void Info::InitFieldIndices()
	{
	if ( file_id_idx != -1 ) return;
	file_id_idx = Idx("file_id");
	parent_file_id_idx = Idx("parent_file_id");
	protocol_idx = Idx("protocol");
	conn_uids_idx = Idx("conn_uids");
	conn_ids_idx = Idx("conn_ids");
	seen_bytes_idx = Idx("seen_bytes");
	total_bytes_idx = Idx("total_bytes");
	missing_bytes_idx = Idx("missing_bytes");
	overflow_bytes_idx = Idx("overflow_bytes");
	timeout_interval_idx = Idx("timeout_interval");
	actions_idx = Idx("actions");
	}

static void action_del_func(void* v)
	{
	delete (Action*) v;
	}

Info::Info(const string& unique, Connection* conn, const string& protocol)
    : file_id(unique), unique(unique), val(0), last_activity_time(network_time),
      postpone_timeout(false), need_reassembly(false)
	{
	InitFieldIndices();

	char id[20];
	uitoa_n(calculate_unique_id(), id, sizeof(id), 62);

	DBG_LOG(DBG_FILE_ANALYSIS, "Creating new Info object %s", id);

	val = new RecordVal(BifType::Record::FileAnalysis::Info);
	val->Assign(file_id_idx, new StringVal(id));
	file_id = FileID(id);

	TypeList* t = new TypeList();
	t->Append(BifType::Record::FileAnalysis::ActionArgs->Ref());
	action_hash = new CompositeHash(t);
	Unref(t);
	action_map.SetDeleteFunc(action_del_func);

	UpdateConnectionFields(conn);

	if ( protocol != "" )
		val->Assign(protocol_idx, new StringVal(protocol.c_str()));

	ScheduleInactivityTimer();
	}

Info::~Info()
	{
	DBG_LOG(DBG_FILE_ANALYSIS, "Destroying Info object %s", file_id.c_str());
	delete action_hash;
	Unref(val);
	}

void Info::UpdateConnectionFields(Connection* conn)
	{
	if ( ! conn ) return;

	Val* conn_uids = val->Lookup(conn_uids_idx);
	Val* conn_ids = val->Lookup(conn_ids_idx);
	if ( ! conn_uids )
		val->Assign(conn_uids_idx, conn_uids = new TableVal(string_set));
	if ( ! conn_ids )
		val->Assign(conn_ids_idx, conn_ids = empty_conn_id_set());

	conn_uids->AsTableVal()->Assign(get_conn_uid_val(conn), 0);
	conn_ids->AsTableVal()->Assign(get_conn_id_val(conn), 0);
	}

uint64 Info::LookupFieldDefaultCount(int idx) const
	{
	Val* v = val->LookupWithDefault(idx);
	uint64 rval = v->AsCount();
	Unref(v);
	return rval;
	}

double Info::LookupFieldDefaultInterval(int idx) const
	{
	Val* v = val->LookupWithDefault(idx);
	double rval = v->AsInterval();
	Unref(v);
	return rval;
	}

int Info::Idx(const string& field)
	{
	int rval = BifType::Record::FileAnalysis::Info->FieldOffset(field.c_str());
	if ( rval < 0 )
		reporter->InternalError("Unkown FileAnalysis::Info field: %s",
		                        field.c_str());
	return rval;
	}

double Info::GetTimeoutInterval() const
	{
	return LookupFieldDefaultInterval(timeout_interval_idx);
	}

RecordVal* Info::GetResults(RecordVal* args) const
	{
	TableVal* actions_table = val->Lookup(actions_idx)->AsTableVal();
	RecordVal* rval = actions_table->Lookup(args)->AsRecordVal();

	if ( ! rval )
		{
		rval = new RecordVal(BifType::Record::FileAnalysis::ActionResults);
		actions_table->Assign(args, rval);
		}

	return rval;
	}

void Info::IncrementByteCount(uint64 size, int field_idx)
	{
	uint64 old = LookupFieldDefaultCount(field_idx);
	val->Assign(field_idx, new Val(old + size, TYPE_COUNT));
	}

void Info::SetTotalBytes(uint64 size)
	{
	val->Assign(total_bytes_idx, new Val(size, TYPE_COUNT));
	}

bool Info::IsComplete() const
	{
	Val* total = val->Lookup(total_bytes_idx);
	if ( ! total ) return false;
	if ( LookupFieldDefaultCount(seen_bytes_idx) >= total->AsCount() )
		return true;
	return false;
	}

void Info::ScheduleInactivityTimer() const
	{
	timer_mgr->Add(new InfoTimer(network_time, file_id, GetTimeoutInterval()));
	}

bool Info::AddAction(RecordVal* args)
	{
	HashKey* key = action_hash->ComputeHash(args, 1);

	if ( ! key )
		reporter->InternalError("ActionArgs type mismatch in add_action");

	Action* act = action_map.Lookup(key);

	if ( act )
		{
		DBG_LOG(DBG_FILE_ANALYSIS, "Add action %d skipped for already active"
		        " action on file id %s", act->Tag(), file_id.c_str());
		delete key;
		return false;
		}

	act = action_factory[Action::ArgsTag(args)](args, this);

	if ( ! act )
		{
		DBG_LOG(DBG_FILE_ANALYSIS, "Failed to instantiate action %d"
		        " on file id %s", Action::ArgsTag(args), file_id.c_str());
		delete key;
		return false;
		}

	DBG_LOG(DBG_FILE_ANALYSIS, "Add action %d for file id %s", act->Tag(),
	        file_id.c_str());

	action_map.Insert(key, act);
	val->Lookup(actions_idx)->AsTableVal()->Assign(args,
	        new RecordVal(BifType::Record::FileAnalysis::ActionResults));

	return true;
	}

void Info::ScheduleRemoval(const Action* act)
	{
	removing.push_back(act->Args());
	}

void Info::DoActionRemoval()
	{
	ActionArgList::iterator it;
	for ( it = removing.begin(); it != removing.end(); ++it )
		RemoveAction(*it);
	removing.clear();
	}

bool Info::RemoveAction(const RecordVal* args)
	{
	HashKey* key = action_hash->ComputeHash(args, 1);

	if ( ! key )
		reporter->InternalError("ActionArgs type mismatch in remove_action");

	Action* act = (Action*) action_map.Remove(key);
	delete key;

	if ( ! act )
		{
		DBG_LOG(DBG_FILE_ANALYSIS, "Skip remove action %d for file id %s",
	            Action::ArgsTag(args), file_id.c_str());
		return false;
		}

	DBG_LOG(DBG_FILE_ANALYSIS, "Remove action %d for file id %s", act->Tag(),
	        file_id.c_str());
	delete act;
	return true;
	}

void Info::DataIn(const u_char* data, uint64 len, uint64 offset)
	{
	Action* act = 0;
	IterCookie* c = action_map.InitForIteration();

	while ( (act = action_map.NextEntry(c)) )
		{
		if ( ! act->DeliverChunk(data, len, offset) )
			ScheduleRemoval(act);
		}

	DoActionRemoval();

	// TODO: check reassembly requirement based on buffer size in record
	if ( need_reassembly )
		{
		// TODO
		}

	// TODO: reassembly stuff, possibly having to deliver chunks if buffer full
	//       and incrememt overflow bytes

	IncrementByteCount(len, seen_bytes_idx);
	}

void Info::DataIn(const u_char* data, uint64 len)
	{
	Action* act = 0;
	IterCookie* c = action_map.InitForIteration();

	while ( (act = action_map.NextEntry(c)) )
		{
		if ( ! act->DeliverStream(data, len) )
			{
			ScheduleRemoval(act);
			continue;
			}

		uint64 offset = LookupFieldDefaultCount(seen_bytes_idx) +
		                LookupFieldDefaultCount(missing_bytes_idx);


		if ( ! act->DeliverChunk(data, len, offset) )
			ScheduleRemoval(act);
		}

	DoActionRemoval();
	IncrementByteCount(len, seen_bytes_idx);
	}

void Info::EndOfFile()
	{
	Action* act = 0;
	IterCookie* c = action_map.InitForIteration();

	while ( (act = action_map.NextEntry(c)) )
		{
		if ( ! act->EndOfFile() )
			ScheduleRemoval(act);
		}

	DoActionRemoval();
	}

void Info::Gap(uint64 offset, uint64 len)
	{
	Action* act = 0;
	IterCookie* c = action_map.InitForIteration();

	while ( (act = action_map.NextEntry(c)) )
		{
		if ( ! act->Undelivered(offset, len) )
			ScheduleRemoval(act);
		}

	DoActionRemoval();
	IncrementByteCount(len, missing_bytes_idx);
	}
