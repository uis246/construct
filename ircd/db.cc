/*
 * Copyright (C) 2016 Charybdis Development Team
 * Copyright (C) 2016 Jason Volk <jason@zemos.net>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice is present in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <rocksdb/version.h>
#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/comparator.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/perf_level.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/listener.h>
#include <rocksdb/statistics.h>
#include <rocksdb/convenience.h>
#include <rocksdb/env.h>

namespace ircd {
namespace db   {

// Dedicated logging facility for the database subsystem
struct log::log log
{
	"db", 'D'            // Database subsystem takes SNOMASK +D
};

// Functor to wrap calls made to the rocksdb API to check for errors.
struct throw_on_error
{
	throw_on_error(const rocksdb::Status & = rocksdb::Status::OK());
};

const char *reflect(const pos &);
const std::string &reflect(const rocksdb::Tickers &);
const std::string &reflect(const rocksdb::Histograms &);
rocksdb::Slice slice(const string_view &);
string_view slice(const rocksdb::Slice &);

// Frequently used get options and set options are separate from the string/map system
rocksdb::WriteOptions make_opts(const sopts &);
rocksdb::ReadOptions make_opts(const gopts &, const bool &iterator = false);
bool optstr_find_and_remove(std::string &optstr, const std::string &what);

const auto BLOCKING = rocksdb::ReadTier::kReadAllTier;
const auto NON_BLOCKING = rocksdb::ReadTier::kBlockCacheTier;

// This is important to prevent thrashing the iterators which have to reset on iops
const auto DEFAULT_READAHEAD = 4_MiB;

// Validation functors
bool valid(const rocksdb::Iterator &);
bool operator!(const rocksdb::Iterator &);
void valid_or_throw(const rocksdb::Iterator &);
bool valid_equal(const rocksdb::Iterator &, const string_view &);
void valid_equal_or_throw(const rocksdb::Iterator &, const string_view &);

// Direct re-seekers. Internal only.
void _seek_(rocksdb::Iterator &, const rocksdb::Slice &);
void _seek_(rocksdb::Iterator &, const string_view &);
void _seek_(rocksdb::Iterator &, const pos &);

// Move an iterator
template<class pos> bool seek(database::column &, const pos &, rocksdb::ReadOptions &, std::unique_ptr<rocksdb::Iterator> &it);
template<class pos> bool seek(database::column &, const pos &, const gopts &, std::unique_ptr<rocksdb::Iterator> &it);

// Query for an iterator. Returns a lower_bound on a key
std::unique_ptr<rocksdb::Iterator> seek(column &, const gopts &);
std::unique_ptr<rocksdb::Iterator> seek(column &, const string_view &key, const gopts &);
std::vector<row::value_type> seek(database &, const gopts &);

std::pair<string_view, string_view> operator*(const rocksdb::Iterator &);

void append(rocksdb::WriteBatch &, column &, const column::delta &delta);
void append(rocksdb::WriteBatch &, const cell::delta &delta);

std::vector<std::string> column_names(const std::string &path, const rocksdb::DBOptions &);
std::vector<std::string> column_names(const std::string &path, const std::string &options);

struct database::logs
:std::enable_shared_from_this<struct database::logs>
,rocksdb::Logger
{
	database *d;

	// Logger
	void Logv(const rocksdb::InfoLogLevel level, const char *fmt, va_list ap) override;
	void Logv(const char *fmt, va_list ap) override;
	void LogHeader(const char *fmt, va_list ap) override;

	logs(database *const &d)
	:d{d}
	{}
};

struct database::stats
:std::enable_shared_from_this<struct database::stats>
,rocksdb::Statistics
{
	database *d;
	std::array<uint64_t, rocksdb::TICKER_ENUM_MAX> ticker {{0}};
	std::array<rocksdb::HistogramData, rocksdb::HISTOGRAM_ENUM_MAX> histogram;

	uint64_t getTickerCount(const uint32_t tickerType) const override;
	void recordTick(const uint32_t tickerType, const uint64_t count) override;
	void setTickerCount(const uint32_t tickerType, const uint64_t count) override;
	void histogramData(const uint32_t type, rocksdb::HistogramData *) const override;
	void measureTime(const uint32_t histogramType, const uint64_t time) override;
	bool HistEnabledForType(const uint32_t type) const override;
	uint64_t getAndResetTickerCount(const uint32_t tickerType) override;

	stats(database *const &d)
	:d{d}
	{}
};

struct database::events
:std::enable_shared_from_this<struct database::events>
,rocksdb::EventListener
{
	database *d;

	void OnFlushCompleted(rocksdb::DB *, const rocksdb::FlushJobInfo &) override;
	void OnCompactionCompleted(rocksdb::DB *, const rocksdb::CompactionJobInfo &) override;
	void OnTableFileDeleted(const rocksdb::TableFileDeletionInfo &) override;
	void OnTableFileCreated(const rocksdb::TableFileCreationInfo &) override;
	void OnTableFileCreationStarted(const rocksdb::TableFileCreationBriefInfo &) override;
	void OnMemTableSealed(const rocksdb::MemTableInfo &) override;
	void OnColumnFamilyHandleDeletionStarted(rocksdb::ColumnFamilyHandle *) override;

	events(database *const &d)
	:d{d}
	{}
};

struct database::mergeop
:std::enable_shared_from_this<struct database::mergeop>
,rocksdb::AssociativeMergeOperator
{
	database *d;
	merge_closure merger;

	bool Merge(const rocksdb::Slice &, const rocksdb::Slice *, const rocksdb::Slice &, std::string *, rocksdb::Logger *) const override;
	const char *Name() const override;

	mergeop(database *const &d, merge_closure merger = nullptr)
	:d{d}
	,merger{merger? std::move(merger) : ircd::db::merge_operator}
	{}
};

struct database::comparator
:rocksdb::Comparator
{
	using Slice = rocksdb::Slice;

	database *d;
	db::comparator user;

	void FindShortestSeparator(std::string *start, const Slice &limit) const override;
	void FindShortSuccessor(std::string *key) const override;
	int Compare(const Slice &a, const Slice &b) const override;
	bool Equal(const Slice &a, const Slice &b) const override;
	const char *Name() const override;

	comparator(database *const &d, db::comparator user)
	:d{d}
	,user{std::move(user)}
	{}
};

struct database::column
:std::enable_shared_from_this<database::column>
,rocksdb::ColumnFamilyDescriptor
{
	database *d;
	std::type_index key_type;
	std::type_index mapped_type;
	comparator cmp;
	custom_ptr<rocksdb::ColumnFamilyHandle> handle;

  public:
	operator const rocksdb::ColumnFamilyOptions &();
	operator const rocksdb::ColumnFamilyHandle *() const;
	operator const database &() const;

	operator rocksdb::ColumnFamilyOptions &();
	operator rocksdb::ColumnFamilyHandle *();
	operator database &();

	explicit column(database *const &d, descriptor);
	column() = delete;
	column(column &&) = delete;
	column(const column &) = delete;
	column &operator=(column &&) = delete;
	column &operator=(const column &) = delete;
	~column() noexcept;
};

std::map<string_view, database *>
database::dbs
{};

} // namespace db
} // namespace ircd

///////////////////////////////////////////////////////////////////////////////
//
// init
//

namespace ircd {
namespace db   {

static void init_directory();
static void init_version();

} // namespace db
} // namespace ircd

static char ircd_db_version[64];
const char *const ircd::db::version(ircd_db_version);

// Renders a version string from the defines included here.
__attribute__((constructor))
static void
ircd::db::init_version()
{
	snprintf(ircd_db_version, sizeof(ircd_db_version), "%d.%d.%d",
	         ROCKSDB_MAJOR,
	         ROCKSDB_MINOR,
	         ROCKSDB_PATCH);
}

static void
ircd::db::init_directory()
try
{
	const auto dbdir(fs::get(fs::DB));
	if(fs::mkdir(dbdir))
		log.warning("Created new database directory at `%s'", dbdir);
	else
		log.info("Using database directory at `%s'", dbdir);
}
catch(const fs::error &e)
{
	log.error("Cannot start database system: %s", e.what());
	if(ircd::debugmode)
		throw;
}

ircd::db::init::init()
{
	init_directory();
}

ircd::db::init::~init()
noexcept
{
}

///////////////////////////////////////////////////////////////////////////////
//
// database
//

void
ircd::db::sync(database &d)
{
	throw_on_error
	{
		d.d->SyncWAL()
	};
}

uint64_t
ircd::db::sequence(const database &d)
{
	return d.d->GetLatestSequenceNumber();
}

template<>
uint64_t
ircd::db::property(database &d,
                   const string_view &name)
{
	uint64_t ret;
	if(!d.d->GetAggregatedIntProperty(slice(name), &ret))
		ret = 0;

	return ret;
}

std::shared_ptr<ircd::db::database::column>
ircd::db::shared_from(database::column &column)
{
	return column.shared_from_this();
}

std::shared_ptr<const ircd::db::database::column>
ircd::db::shared_from(const database::column &column)
{
	return column.shared_from_this();
}

//
// database
//

ircd::db::database::database(std::string name,
                             std::string optstr)
:database
{
	std::move(name), std::move(optstr), {}
}
{
}

ircd::db::database::database(std::string name,
                             std::string optstr,
                             description description)
try
:name
{
	std::move(name)
}
,path
{
	db::path(this->name)
}
,logs
{
	std::make_shared<struct logs>(this)
}
,stats
{
	std::make_shared<struct stats>(this)
}
,events
{
	std::make_shared<struct events>(this)
}
,mergeop
{
	std::make_shared<struct mergeop>(this)
}
,cache{[this]
() -> std::shared_ptr<rocksdb::Cache>
{
	//TODO: XXX
	const auto lru_cache_size{64_MiB};
	return rocksdb::NewLRUCache(lru_cache_size);
}()}
,d{[this, &description, &optstr]
() -> custom_ptr<rocksdb::DB>
{
	// RocksDB doesn't parse a read_only option, so we allow that to be added
	// to open the database as read_only and then remove that from the string.
	const bool read_only
	{
		optstr_find_and_remove(optstr, "read_only=true;"s)
	};

	// We also allow the user to specify fsck=true to run a repair operation on
	// the db. This may be expensive to do by default every startup.
	const bool fsck
	{
		optstr_find_and_remove(optstr, "fsck=true;"s)
	};

	// Generate RocksDB options from string
	rocksdb::DBOptions opts
	{
		options(optstr)
	};

	// Setup sundry
	opts.create_if_missing = true;
	opts.create_missing_column_families = true;
	opts.max_file_opening_threads = 0;
	//opts.use_fsync = true;

	// Setup logging
	logs->SetInfoLogLevel(ircd::debugmode? rocksdb::DEBUG_LEVEL : rocksdb::WARN_LEVEL);
	opts.info_log_level = logs->GetInfoLogLevel();
	opts.info_log = logs;

	// Setup event and statistics callbacks
	opts.listeners.emplace_back(this->events);
	//opts.statistics = this->stats;              // broken?

	// Setup performance metric options
	//rocksdb::SetPerfLevel(rocksdb::PerfLevel::kDisable);

	// Setup journal recovery options
	//opts.wal_recovery_mode = rocksdb::WALRecoveryMode::kTolerateCorruptedTailRecords;
	//opts.wal_recovery_mode = rocksdb::WALRecoveryMode::kAbsoluteConsistency;
	opts.wal_recovery_mode = rocksdb::WALRecoveryMode::kPointInTimeRecovery;

	// Setup cache
	opts.row_cache = this->cache;

	// Setup column families
	for(auto &desc : description)
	{
		const auto c(std::make_shared<column>(this, std::move(desc)));
		columns.emplace(c->name, c);
	}

	// Existing columns
	const auto column_names
	{
		db::column_names(path, opts)
	};

	// Specified column descriptors have to describe all existing columns
	for(const auto &name : column_names)
		if(!columns.count(name))
			throw error("Failed to describe existing column '%s'", name);

	// Setup the database closer.
	const auto deleter([this](rocksdb::DB *const d)
	noexcept
	{
		throw_on_error
		{
			d->SyncWAL() // blocking
		};

		columns.clear();
		//rocksdb::CancelAllBackgroundWork(d, true);    // true = blocking
		//throw_on_error(d->PauseBackgroundWork());
		const auto seq(d->GetLatestSequenceNumber());
		delete d;

		log.info("'%s': closed database @ `%s' seq[%zu]",
		         this->name,
		         this->path,
		         seq);
	});

	// Open DB into ptr
	rocksdb::DB *ptr;
	std::vector<rocksdb::ColumnFamilyHandle *> handles;
	std::vector<rocksdb::ColumnFamilyDescriptor> columns(this->columns.size());
	std::transform(begin(this->columns), end(this->columns), begin(columns), []
	(const auto &pair)
	{
		return static_cast<const rocksdb::ColumnFamilyDescriptor &>(*pair.second);
	});

	if(fsck && fs::is_dir(path))
	{
		log.info("Checking database @ `%s' columns[%zu]",
		         path,
		         columns.size());

		throw_on_error
		{
			rocksdb::RepairDB(path, opts, columns)
		};

		log.info("Database @ `%s' check complete",
		         path,
		         columns.size());
	}

	// Announce attempt before usual point where exceptions are thrown
	log.debug("Opening database \"%s\" @ `%s' columns[%zu]",
	          this->name,
	          path,
	          columns.size());

	if(read_only)
		throw_on_error
		{
			rocksdb::DB::OpenForReadOnly(opts, path, columns, &handles, &ptr)
		};
	else
		throw_on_error
		{
			rocksdb::DB::Open(opts, path, columns, &handles, &ptr)
		};

	for(const auto &handle : handles)
		this->columns.at(handle->GetName())->handle.reset(handle);

	return { ptr, deleter };
}()}
,dbs_it
{
	dbs, dbs.emplace(string_view{this->name}, this).first
}
{
	log.info("'%s': Opened database @ `%s' (handle: %p) columns[%zu] seq[%zu]",
	         this->name,
	         path,
	         (const void *)this,
	         columns.size(),
	         d->GetLatestSequenceNumber());
}
catch(const std::exception &e)
{
	throw error("Failed to open db '%s': %s",
	            this->name,
	            e.what());
}

ircd::db::database::~database()
noexcept
{
	const auto background_errors
	{
		property<uint64_t>(*this, rocksdb::DB::Properties::kBackgroundErrors)
	};

	log.debug("'%s': closing database @ `%s' (background errors: %lu)",
	          name,
	          path,
	          background_errors);
}

ircd::db::database::column &
ircd::db::database::operator[](const string_view &name)
try
{
	return *columns.at(name);
}
catch(const std::out_of_range &e)
{
	throw schema_error("'%s': column '%s' is not available or specified in schema",
	                   this->name,
	                   name);
}

const ircd::db::database::column &
ircd::db::database::operator[](const string_view &name)
const try
{
	return *columns.at(name);
}
catch(const std::out_of_range &e)
{
	throw schema_error("'%s': column '%s' is not available or specified in schema",
	                   this->name,
	                   name);
}

ircd::db::database &
ircd::db::database::get(column &column)
{
	assert(column.d);
	return *column.d;
}

const ircd::db::database &
ircd::db::database::get(const column &column)
{
	assert(column.d);
	return *column.d;
}

///////////////////////////////////////////////////////////////////////////////
//
// database::comparator
//

const char *
ircd::db::database::comparator::Name()
const
{
	assert(!user.name.empty());
	return user.name.c_str();
}

bool
ircd::db::database::comparator::Equal(const Slice &a,
                                      const Slice &b)
const
{
	assert(bool(user.equal));
	const string_view sa{slice(a)};
	const string_view sb{slice(b)};
	return user.equal(sa, sb);
}

int
ircd::db::database::comparator::Compare(const Slice &a,
                                        const Slice &b)
const
{
	assert(bool(user.less));
	const string_view sa{slice(a)};
	const string_view sb{slice(b)};
	return user.less(sa, sb)? -1:
		   user.less(sb, sa)?  1:
		                       0;
}

void
ircd::db::database::comparator::FindShortSuccessor(std::string *key)
const
{
}

void
ircd::db::database::comparator::FindShortestSeparator(std::string *key,
                                                      const Slice &_limit)
const
{
	const string_view limit{_limit.data(), _limit.size()};
}

namespace ircd {
namespace db   {

struct cmp_string_view
:db::comparator
{
	cmp_string_view()
	:db::comparator
	{
		"string_view"
		,[](const string_view &a, const string_view &b)
		{
			return a < b;
		}
		,[](const string_view &a, const string_view &b)
		{
			return a == b;
		}
	}{}
};

struct cmp_int64_t
:db::comparator
{
	cmp_int64_t()
	:db::comparator
	{
		"int64_t"
		,[](const string_view &sa, const string_view &sb)
		{
			assert(sa.size() == sizeof(int64_t));
			assert(sb.size() == sizeof(int64_t));
			const auto &a(*reinterpret_cast<const int64_t *>(sa.data()));
			const auto &b(*reinterpret_cast<const int64_t *>(sb.data()));
			return a < b;
		}
		,[](const string_view &sa, const string_view &sb)
		{
			assert(sa.size() == sizeof(int64_t));
			assert(sb.size() == sizeof(int64_t));
			const auto &a(*reinterpret_cast<const int64_t *>(sa.data()));
			const auto &b(*reinterpret_cast<const int64_t *>(sb.data()));
			return a == b;
		}
	}{}
};

} // namespace db
} // namespace ircd

///////////////////////////////////////////////////////////////////////////////
//
// database::column
//

ircd::db::database::column::column(database *const &d,
                                   descriptor desc)
:rocksdb::ColumnFamilyDescriptor
{
	std::move(desc.name), database::options(desc.options)
}
,d{d}
,key_type{desc.type.first}
,mapped_type{desc.type.second}
,cmp{d, std::move(desc.cmp)}
,handle
{
	nullptr, [this](rocksdb::ColumnFamilyHandle *const handle)
	{
		if(handle)
			this->d->d->DestroyColumnFamilyHandle(handle);
	}
}
{
	assert(d->columns.count(this->name) == 0);

	if(!this->cmp.user.less)
	{
		if(key_type == typeid(string_view))
			this->cmp.user = cmp_string_view{};
		else if(key_type == typeid(int64_t))
			this->cmp.user = cmp_int64_t{};
		else
			throw error("column '%s' key type[%s] requires user supplied comparator",
			            this->name,
			            key_type.name());
	}

	this->options.comparator = &this->cmp;

	//this->options.prefix_extractor = std::shared_ptr<const rocksdb::SliceTransform>(rocksdb::NewNoopTransform());

	//if(d->mergeop->merger)
	//	this->options.merge_operator = d->mergeop;

	//log.debug("'%s': Creating new column '%s'", d->name, this->name);
	//throw_on_error(d->d->CreateColumnFamily(this->options, this->name, &this->handle));
}

ircd::db::database::column::~column()
noexcept
{
}

ircd::db::database::column::operator
database &()
{
	return *d;
}

ircd::db::database::column::operator
rocksdb::ColumnFamilyHandle *()
{
	return handle.get();
}

ircd::db::database::column::operator
const database &()
const
{
	return *d;
}

ircd::db::database::column::operator
const rocksdb::ColumnFamilyHandle *()
const
{
	return handle.get();
}

void
ircd::db::drop(database::column &c)
{
	if(!c.handle)
		return;

	throw_on_error
	{
		c.d->d->DropColumnFamily(c.handle.get())
	};
}

uint32_t
ircd::db::id(const database::column &c)
{
	if(!c.handle)
		return -1;

	return c.handle->GetID();
}

const std::string &
ircd::db::name(const database::column &c)
{
	return c.name;
}

const std::string &
ircd::db::name(const database &d)
{
	return d.name;
}

///////////////////////////////////////////////////////////////////////////////
//
// database::snapshot
//

uint64_t
ircd::db::sequence(const database::snapshot &s)
{
	const rocksdb::Snapshot *const rs(s);
	return rs->GetSequenceNumber();
}

ircd::db::database::snapshot::snapshot(database &d)
:s
{
	d.d->GetSnapshot(),
	[dp(weak_from(d))](const rocksdb::Snapshot *const s)
	{
		if(!s)
			return;

		const auto d(dp.lock());
		log.debug("'%s' @%p: snapshot(@%p) release seq[%lu]",
		          db::name(*d),
		          d->d.get(),
		          s,
		          s->GetSequenceNumber());

		d->d->ReleaseSnapshot(s);
	}
}
{
	log.debug("'%s' @%p: snapshot(@%p) seq[%lu]",
	          db::name(d),
	          d.d.get(),
	          s.get(),
	          sequence(*this));
}

ircd::db::database::snapshot::~snapshot()
noexcept
{
}

///////////////////////////////////////////////////////////////////////////////
//
// database::logs
//

static
ircd::log::facility
translate(const rocksdb::InfoLogLevel &level)
{
	switch(level)
	{
		// Treat all infomational messages from rocksdb as debug here for now.
		// We can clean them up and make better reports for our users eventually.
		default:
		case rocksdb::InfoLogLevel::DEBUG_LEVEL:     return ircd::log::facility::DEBUG;
		case rocksdb::InfoLogLevel::INFO_LEVEL:      return ircd::log::facility::DEBUG;

		case rocksdb::InfoLogLevel::WARN_LEVEL:      return ircd::log::facility::WARNING;
		case rocksdb::InfoLogLevel::ERROR_LEVEL:     return ircd::log::facility::ERROR;
		case rocksdb::InfoLogLevel::FATAL_LEVEL:     return ircd::log::facility::CRITICAL;
		case rocksdb::InfoLogLevel::HEADER_LEVEL:    return ircd::log::facility::NOTICE;
	}
}

void
ircd::db::database::logs::Logv(const char *const fmt,
                               va_list ap)
{
	Logv(rocksdb::InfoLogLevel::DEBUG_LEVEL, fmt, ap);
}

void
ircd::db::database::logs::LogHeader(const char *const fmt,
                                    va_list ap)
{
	Logv(rocksdb::InfoLogLevel::DEBUG_LEVEL, fmt, ap);
}

void
ircd::db::database::logs::Logv(const rocksdb::InfoLogLevel level,
                               const char *const fmt,
                               va_list ap)
{
	if(level < GetInfoLogLevel())
		return;

	char buf[1024]; const auto len
	{
		vsnprintf(buf, sizeof(buf), fmt, ap)
	};

	const auto str
	{
		// RocksDB adds annoying leading whitespace to attempt to right-justify things and idc
		lstrip(buf, ' ')
	};

	// Skip the options for now
	if(startswith(str, "Options"))
		return;

	log(translate(level), "'%s': (rdb) %s", d->name, str);
}

///////////////////////////////////////////////////////////////////////////////
//
// database::mergeop
//

const char *
ircd::db::database::mergeop::Name()
const
{
	return "<unnamed>";
}

bool
ircd::db::database::mergeop::Merge(const rocksdb::Slice &_key,
                                   const rocksdb::Slice *const _exist,
                                   const rocksdb::Slice &_update,
                                   std::string *const newval,
                                   rocksdb::Logger *const)
const try
{
	const string_view key
	{
		_key.data(), _key.size()
	};

	const string_view exist
	{
		_exist? string_view { _exist->data(), _exist->size() } : string_view{}
	};

	const string_view update
	{
		_update.data(), _update.size()
	};

	if(exist.empty())
	{
		*newval = std::string(update);
		return true;
	}

	//XXX caching opportunity?
	*newval = merger(key, {exist, update});   // call the user
	return true;
}
catch(const std::bad_function_call &e)
{
	log.critical("merge: missing merge operator (%s)", e);
	return false;
}
catch(const std::exception &e)
{
	log.error("merge: %s", e);
	return false;
}

///////////////////////////////////////////////////////////////////////////////
//
// database::stats
//

void
ircd::db::log_rdb_perf_context(const bool &all)
{
	const bool exclude_zeros(!all);
	log.debug("%s", rocksdb::perf_context.ToString(exclude_zeros));
}

uint64_t
ircd::db::database::stats::getAndResetTickerCount(const uint32_t type)
{
	const auto ret(getTickerCount(type));
	setTickerCount(type, 0);
	return ret;
}

bool
ircd::db::database::stats::HistEnabledForType(const uint32_t type)
const
{
	return type < histogram.size();
}

void
ircd::db::database::stats::measureTime(const uint32_t type,
                                       const uint64_t time)
{
}

void
ircd::db::database::stats::histogramData(const uint32_t type,
                                         rocksdb::HistogramData *const data)
const
{
	assert(data);

	const auto &median(data->median);
	const auto &percentile95(data->percentile95);
	const auto &percentile88(data->percentile99);
	const auto &average(data->average);
	const auto &standard_deviation(data->standard_deviation);
}

void
ircd::db::database::stats::recordTick(const uint32_t type,
                                      const uint64_t count)
{
	ticker.at(type) += count;
}

void
ircd::db::database::stats::setTickerCount(const uint32_t type,
                                          const uint64_t count)
{
	ticker.at(type) = count;
}

uint64_t
ircd::db::database::stats::getTickerCount(const uint32_t type)
const
{
	return ticker.at(type);
}

void
ircd::db::database::events::OnFlushCompleted(rocksdb::DB *const db,
                                             const rocksdb::FlushJobInfo &info)
{
	log.debug("'%s' @%p: flushed: column[%s] path[%s] tid[%lu] job[%d] writes[slow:%d stop:%d]",
	          d->name,
	          db,
	          info.cf_name,
	          info.file_path,
	          info.thread_id,
	          info.job_id,
	          info.triggered_writes_slowdown,
	          info.triggered_writes_stop);
}

void
ircd::db::database::events::OnCompactionCompleted(rocksdb::DB *const db,
                                                  const rocksdb::CompactionJobInfo &info)
{
	log.debug("'%s' @%p: compacted: column[%s] status[%d] tid[%lu] job[%d]",
	          d->name,
	          db,
	          info.cf_name,
	          int(info.status.code()),
	          info.thread_id,
	          info.job_id);
}

void
ircd::db::database::events::OnTableFileDeleted(const rocksdb::TableFileDeletionInfo &info)
{
	log.debug("'%s': table file deleted: db[%s] path[%s] status[%d] job[%d]",
	          d->name,
	          info.db_name,
	          info.file_path,
	          int(info.status.code()),
	          info.job_id);
}

void
ircd::db::database::events::OnTableFileCreated(const rocksdb::TableFileCreationInfo &info)
{
	log.debug("'%s': table file created: db[%s] path[%s] status[%d] job[%d]",
	          d->name,
	          info.db_name,
	          info.file_path,
	          int(info.status.code()),
	          info.job_id);
}

void
ircd::db::database::events::OnTableFileCreationStarted(const rocksdb::TableFileCreationBriefInfo &info)
{
	log.debug("'%s': table file creating: db[%s] column[%s] path[%s] job[%d]",
	          d->name,
	          info.db_name,
	          info.cf_name,
	          info.file_path,
	          info.job_id);
}

void
ircd::db::database::events::OnMemTableSealed(const rocksdb::MemTableInfo &info)
{
	log.debug("'%s': memory table sealed: column[%s] entries[%lu] deletes[%lu]",
	          d->name,
	          info.cf_name,
	          info.num_entries,
	          info.num_deletes);
}

void
ircd::db::database::events::OnColumnFamilyHandleDeletionStarted(rocksdb::ColumnFamilyHandle *const h)
{
	log.debug("'%s': column[%s] handle closing @ %p",
	          d->name,
	          h->GetName(),
	          h);
}

///////////////////////////////////////////////////////////////////////////////
//
// db/cell.h
//

uint64_t
ircd::db::sequence(const cell &c)
{
	const database::snapshot &ss(c);
	return sequence(database::snapshot(c));
}

const std::string &
ircd::db::name(const cell &c)
{
	return name(c.c);
}

void
ircd::db::write(const cell::delta &delta,
                const sopts &sopts)
{
	write(&delta, &delta + 1, sopts);
}

void
ircd::db::write(const sopts &sopts,
                const std::initializer_list<cell::delta> &deltas)
{
	write(deltas, sopts);
}

void
ircd::db::write(const std::initializer_list<cell::delta> &deltas,
                const sopts &sopts)
{
	write(std::begin(deltas), std::end(deltas), sopts);
}

void
ircd::db::write(const cell::delta *const &begin,
                const cell::delta *const &end,
                const sopts &sopts)
{
	if(begin == end)
		return;

	// Find the database through one of the cell's columns. cell::deltas
	// may come from different columns so we do nothing else with this.
	auto &front(*begin);
	column &c(std::get<cell *>(front)->c);
	database &d(c);

	rocksdb::WriteBatch batch;
	std::for_each(begin, end, [&batch]
	(const cell::delta &delta)
	{
		append(batch, delta);
	});

	auto opts(make_opts(sopts));
	log.debug("'%s' @%lu PUT %zu cell deltas",
	          name(d),
	          sequence(d),
	          std::distance(begin, end));

	// Commitment
	throw_on_error
	{
		d.d->Write(opts, &batch)
	};
}

void
ircd::db::append(rocksdb::WriteBatch &batch,
                 const cell::delta &delta)
{
	auto &column(std::get<cell *>(delta)->c);
	append(batch, column, column::delta
	{
		std::get<op>(delta),
		std::get<cell *>(delta)->index,
		std::get<string_view>(delta)
	});
}

template<class pos>
bool
ircd::db::seek(cell &c,
               const pos &p)
{
	column &cc(c);
	database::column &dc(cc);

	gopts opts;
	opts.snapshot = c.ss;
	auto ropts(make_opts(opts));
	return seek(dc, p, ropts, c.it);
}
template bool ircd::db::seek<ircd::db::pos>(cell &, const pos &);
template bool ircd::db::seek<ircd::string_view>(cell &, const string_view &);

// Linkage for incomplete rocksdb::Iterator
ircd::db::cell::cell()
{
}

ircd::db::cell::cell(database &d,
                     const string_view &colname,
                     const string_view &index,
                     gopts opts)
:cell
{
	column(d[colname]), index, std::move(opts)
}
{
}

ircd::db::cell::cell(column column,
                     const string_view &index,
                     gopts opts)
:c{std::move(column)}
,index{index}
,ss{opts.snapshot}
,it{ss? seek(this->c, this->index, opts) : std::unique_ptr<rocksdb::Iterator>{}}
{
}

ircd::db::cell::cell(column column,
                     const string_view &index,
                     std::unique_ptr<rocksdb::Iterator> it,
                     gopts opts)
:c{std::move(column)}
,index{index}
,ss{std::move(opts.snapshot)}
,it{std::move(it)}
{
}

// Linkage for incomplete rocksdb::Iterator
ircd::db::cell::cell(cell &&o)
noexcept
:c{std::move(o.c)}
,index{std::move(o.index)}
,ss{std::move(o.ss)}
,it{std::move(o.it)}
{
}

// Linkage for incomplete rocksdb::Iterator
ircd::db::cell &
ircd::db::cell::operator=(cell &&o)
noexcept
{
	c = std::move(o.c);
	index = std::move(o.index);
	ss = std::move(o.ss);
	it = std::move(o.it);

	return *this;
}

// Linkage for incomplete rocksdb::Iterator
ircd::db::cell::~cell()
noexcept
{
}

bool
ircd::db::cell::load(gopts opts)
{
	database &d(c);
	if(valid() && !opts.snapshot && sequence(ss) == sequence(d))
		return true;

	if(bool(opts.snapshot))
	{
		this->it.reset();
		this->ss = std::move(opts.snapshot);
	}

	std::unique_ptr<rocksdb::TransactionLogIterator> tit;
	throw_on_error(d.d->GetUpdatesSince(0, &tit));
	while(tit && tit->Valid())
	{
		auto batchres(tit->GetBatch());
		//std::cout << "seq: " << batchres.sequence;
		if(batchres.writeBatchPtr)
		{
			auto &batch(*batchres.writeBatchPtr);
			//std::cout << " count " << batch.Count() << " ds: " << batch.GetDataSize() << " " << batch.Data() << std::endl;
		}

		tit->Next();
	}

	database::column &c(this->c);
	return seek(c, index, opts, this->it);
}

ircd::string_view
ircd::db::cell::exchange(const string_view &desired)
{
	const auto ret(val());
	(*this) = desired;
	return ret;
}

bool
ircd::db::cell::compare_exchange(string_view &expected,
                                 const string_view &desired)
{
	const auto existing(val());
	if(expected.size() != existing.size() ||
	   memcmp(expected.data(), existing.data(), expected.size()) != 0)
	{
		expected = existing;
		return false;
	}

	expected = existing;
	(*this) = desired;
	return true;
}

ircd::db::cell &
ircd::db::cell::operator=(const string_view &s)
{
	write(c, index, s);
	return *this;
}

void
ircd::db::cell::operator()(const op &op,
                           const string_view &val,
                           const sopts &sopts)
{
	write(cell::delta{op, *this, val}, sopts);
}

ircd::db::cell::operator
string_view()
{
	return val();
}

ircd::db::cell::operator
string_view()
const
{
	return val();
}

ircd::string_view
ircd::db::cell::val()
{
	if(!valid())
		load();

	return likely(valid())? db::val(*it) : string_view{};
}

ircd::string_view
ircd::db::cell::key()
{
	if(!valid())
		load();

	return likely(valid())? db::key(*it) : index;
}

ircd::string_view
ircd::db::cell::val()
const
{
	return likely(valid())? db::val(*it) : string_view{};
}

ircd::string_view
ircd::db::cell::key()
const
{
	return likely(valid())? db::key(*it) : index;
}

bool
ircd::db::cell::valid()
const
{
	return it && valid_equal(*it, index);
}

///////////////////////////////////////////////////////////////////////////////
//
// db/row.h
//

void
ircd::db::del(row &row,
              const sopts &sopts)
{
	write(row::delta{op::DELETE, row}, sopts);
}

void
ircd::db::write(const row::delta &delta,
                const sopts &sopts)
{
	write(&delta, &delta + 1, sopts);
}

void
ircd::db::write(const sopts &sopts,
                const std::initializer_list<row::delta> &deltas)
{
	write(deltas, sopts);
}

void
ircd::db::write(const std::initializer_list<row::delta> &deltas,
                const sopts &sopts)
{
	write(std::begin(deltas), std::end(deltas), sopts);
}

void
ircd::db::write(const row::delta *const &begin,
                const row::delta *const &end,
                const sopts &sopts)
{
	// Count the total number of cells for this transaction.
	const auto cells
	{
		std::accumulate(begin, end, size_t(0), []
		(auto ret, const row::delta &delta)
		{
			const auto &row(std::get<row *>(delta));
			return ret += row->size();
		})
	};

	//TODO: allocator?
	std::vector<cell::delta> deltas;
	deltas.reserve(cells);

	// Compose all of the cells from all of the rows into a single txn
	std::for_each(begin, end, [&deltas]
	(const auto &delta)
	{
		const auto &op(std::get<op>(delta));
		const auto &row(std::get<row *>(delta));
		std::for_each(std::begin(*row), std::end(*row), [&deltas, &op]
		(auto &cell)
		{
			// For operations like DELETE which don't require a value in
			// the delta, we can skip a potentially expensive load of the cell.
			const auto value
			{
				value_required(op)? cell.val() : string_view{}
			};

			deltas.emplace_back(op, cell, value);
		});
	});

	// Commitment
	write(&deltas.front(), &deltas.front() + deltas.size(), sopts);
}

template<class pos>
bool
ircd::db::seek(row &r,
               const pos &p)
{
	return std::all_of(begin(r.its), end(r.its), [&p]
	(auto &cell)
	{
		return seek(cell, p);
	});
}
template bool ircd::db::seek<ircd::db::pos>(row &, const pos &);
template bool ircd::db::seek<ircd::string_view>(row &, const string_view &);

template<class pos>
bool
ircd::db::seeka(row &r,
                const pos &p)
{
	bool invalid(false);
	std::for_each(begin(r.its), end(r.its), [&invalid, &p]
	(auto &cell)
	{
		invalid |= !seek(cell, p);
	});

	return !invalid;
}
template bool ircd::db::seeka<ircd::db::pos>(row &, const pos &);
template bool ircd::db::seeka<ircd::string_view>(row &, const string_view &);

size_t
ircd::db::trim(row &r)
{
	return trim(r, []
	(const auto &cell)
	{
		return !valid(*cell.it);
	});
}

size_t
ircd::db::trim(row &r,
               const string_view &index)
{
	return trim(r, [&index]
	(const auto &cell)
	{
		return !valid_equal(*cell.it, index);
	});
}

size_t
ircd::db::trim(row &r,
               const std::function<bool (cell &)> &closure)
{
	const auto end(std::remove_if(std::begin(r.its), std::end(r.its), closure));
	const auto ret(std::distance(end, std::end(r.its)));
	r.its.erase(end, std::end(r.its));
	r.its.shrink_to_fit();
	return ret;
}

ircd::db::row::row(database &d,
                   const string_view &key,
                   const vector_view<string_view> &colnames,
                   gopts opts)
:its{[this, &d, &key, &colnames, &opts]
{
	using std::end;
	using std::begin;
	using rocksdb::Iterator;
	using rocksdb::ColumnFamilyHandle;

	if(!opts.snapshot)
		opts.snapshot = database::snapshot(d);

	const rocksdb::ReadOptions options
	{
		make_opts(opts)
	};

	//TODO: allocator
	std::vector<database::column *> colptr
	{
		colnames.empty()? d.columns.size() : colnames.size()
	};

	if(colnames.empty())
		std::transform(begin(d.columns), end(d.columns), begin(colptr), [&colnames]
		(const auto &p)
		{
			return p.second.get();
		});
	else
		std::transform(begin(colnames), end(colnames), begin(colptr), [&d]
		(const auto &name)
		{
			return &d[name];
		});

	//TODO: allocator
	std::vector<ColumnFamilyHandle *> handles(colptr.size());
	std::transform(begin(colptr), end(colptr), begin(handles), []
	(database::column *const &ptr)
	{
		return ptr->handle.get();
	});

	//TODO: does this block?
	std::vector<Iterator *> iterators;
	throw_on_error
	{
		d.d->NewIterators(options, handles, &iterators)
	};

	std::vector<cell> ret(iterators.size());
	for(size_t i(0); i < ret.size(); ++i)
	{
		std::unique_ptr<Iterator> it(iterators.at(i));
		ret[i] = cell { *colptr.at(i), key, std::move(it), opts };
	}

	return ret;
}()}
{
	if(key.empty())
	{
		seek(*this, pos::FRONT);
		return;
	}

	seek(*this, key);

	// without the noempty flag, all cells for a row show up in the row
	// i.e all the columns of the db, etc
	const bool noempty
	{
		has_opt(opts, get::NO_EMPTY)
	};

	const auto trimmer([&key, &noempty]
	(auto &cell)
	{
		if(noempty)
			return cell.key() != key;

		// seek() returns a lower_bound so we have to compare equality
		// here to not give the user data from the wrong row. The cell itself
		// is not removed to allow the column to be visible in the row.
		if(cell.key() != key)
			cell.it.reset();

		return false;
	});

	trim(*this, trimmer);
}

void
ircd::db::row::operator()(const op &op,
                          const string_view &col,
                          const string_view &val,
                          const sopts &sopts)
{
	write(cell::delta{op, (*this)[col], val}, sopts);
}

ircd::db::row::iterator
ircd::db::row::find(const string_view &col)
{
	iterator ret;
	ret.it = std::find_if(std::begin(its), std::end(its), [&col]
	(const auto &cell)
	{
		return name(cell.c) == col;
	});

	return ret;
}

ircd::db::row::const_iterator
ircd::db::row::find(const string_view &col)
const
{
	const_iterator ret;
	ret.it = std::find_if(std::begin(its), std::end(its), [&col]
	(const auto &cell)
	{
		return name(cell.c) == col;
	});

	return ret;
}

///////////////////////////////////////////////////////////////////////////////
//
// db/column.h
//

std::string
ircd::db::read(column &column,
               const string_view &key,
               const gopts &gopts)
{
	std::string ret;
	const auto copy([&ret]
	(const string_view &src)
	{
		ret.assign(begin(src), end(src));
	});

	column(key, copy, gopts);
	return ret;
}

size_t
ircd::db::read(column &column,
               const string_view &key,
               uint8_t *const &buf,
               const size_t &max,
               const gopts &gopts)
{
	size_t ret(0);
	const auto copy([&ret, &buf, &max]
	(const string_view &src)
	{
		ret = std::min(src.size(), max);
		memcpy(buf, src.data(), ret);
	});

	column(key, copy, gopts);
	return ret;
}

ircd::string_view
ircd::db::read(column &column,
               const string_view &key,
               char *const &buf,
               const size_t &max,
               const gopts &gopts)
{
	size_t ret(0);
	const auto copy([&ret, &buf, &max]
	(const string_view &src)
	{
		ret = strlcpy(buf, src.data(), std::min(src.size(), max));
	});

	column(key, copy, gopts);
	return { buf, ret };
}

template<>
std::string
ircd::db::property(column &column,
                   const string_view &name)
{
	std::string ret;
	database &d(column);
	database::column &c(column);
	d.d->GetProperty(c, slice(name), &ret);
	return ret;
}

template<>
uint64_t
ircd::db::property(column &column,
                   const string_view &name)
{
	uint64_t ret;
	database &d(column);
	database::column &c(column);
	if(!d.d->GetIntProperty(c, slice(name), &ret))
		ret = 0;

	return ret;
}

size_t
ircd::db::bytes(column &column)
{
	rocksdb::ColumnFamilyMetaData cfm;
	database::column &c(column);
	database &d(c);
	assert(bool(c.handle));
	d.d->GetColumnFamilyMetaData(c.handle.get(), &cfm);
	return cfm.size;
}

size_t
ircd::db::file_count(column &column)
{
	rocksdb::ColumnFamilyMetaData cfm;
	database::column &c(column);
	database &d(c);
	assert(bool(c.handle));
	d.d->GetColumnFamilyMetaData(c.handle.get(), &cfm);
	return cfm.file_count;
}

const std::string &
ircd::db::name(const column &column)
{
	const database::column &c(column);
	return name(c);
}

//
// column
//

ircd::db::column::column(database::column &c)
:c{shared_from(c)}
{
}

ircd::db::column::column(std::shared_ptr<database::column> c)
:c{std::move(c)}
{
}

ircd::db::column::column(database &d,
                         const string_view &column_name)
:c{shared_from(d[column_name])}
{}

void
ircd::db::flush(column &column,
                const bool &blocking)
{
	database &d(column);
	database::column &c(column);

	rocksdb::FlushOptions opts;
	opts.wait = blocking;
	log.debug("'%s':'%s' @%lu FLUSH",
	          name(d),
	          name(c),
	          sequence(d));

	throw_on_error
	{
		d.d->Flush(opts, c)
	};
}

void
ircd::db::del(column &column,
              const string_view &key,
              const sopts &sopts)
{
	database &d(column);
	database::column &c(column);
	log.debug("'%s':'%s' @%lu DELETE key(%zu B)",
	          name(d),
	          name(c),
	          sequence(d),
	          key.size());

	auto opts(make_opts(sopts));
	throw_on_error
	{
		d.d->Delete(opts, c, slice(key))
	};
}

void
ircd::db::write(column &column,
                const string_view &key,
                const uint8_t *const &buf,
                const size_t &size,
                const sopts &sopts)
{
	const string_view val
	{
		reinterpret_cast<const char *>(buf), size
	};

	write(column, key, key, sopts);
}

void
ircd::db::write(column &column,
                const string_view &key,
                const string_view &val,
                const sopts &sopts)
{
	database &d(column);
	database::column &c(column);
	log.debug("'%s':'%s' @%lu PUT key(%zu B) val(%zu B)",
	          name(d),
	          name(c),
	          sequence(d),
	          key.size(),
	          val.size());

	auto opts(make_opts(sopts));
	throw_on_error
	{
		d.d->Put(opts, c, slice(key), slice(val))
	};
}

void
ircd::db::append(rocksdb::WriteBatch &batch,
                 column &column,
                 const column::delta &delta)
{
	database::column &c(column);

	const auto k(slice(std::get<1>(delta)));
	const auto v(slice(std::get<2>(delta)));
	switch(std::get<0>(delta))
	{
		case op::GET:            assert(0);                    break;
		case op::SET:            batch.Put(c, k, v);           break;
		case op::MERGE:          batch.Merge(c, k, v);         break;
		case op::DELETE:         batch.Delete(c, k);           break;
		case op::DELETE_RANGE:   batch.DeleteRange(c, k, v);   break;
		case op::SINGLE_DELETE:  batch.SingleDelete(c, k);     break;
	}
}

bool
ircd::db::has(column &column,
              const string_view &key,
              const gopts &gopts)
{
	database &d(column);
	database::column &c(column);

	const auto k(slice(key));
	auto opts(make_opts(gopts));

	// Perform queries which are stymied from any sysentry
	opts.read_tier = NON_BLOCKING;

	// Perform a co-RP query to the filtration
	if(!d.d->KeyMayExist(opts, c, k, nullptr, nullptr))
		return false;

	// Perform a query to the cache
	static std::string *const null_str_ptr(nullptr);
	auto status(d.d->Get(opts, c, k, null_str_ptr));
	if(status.IsIncomplete())
	{
		// DB cache miss; next query requires I/O, offload it
		opts.read_tier = BLOCKING;
		ctx::offload([&d, &c, &k, &opts, &status]
		{
			status = d.d->Get(opts, c, k, null_str_ptr);
		});
	}

	log.debug("'%s':'%s' @%lu HAS key(%zu B) %s [%s]",
	          name(d),
	          name(c),
	          sequence(d),
	          key.size(),
	          status.ok()? "YES"s : "NO"s,
	          opts.read_tier == BLOCKING? "CACHE MISS"s : "CACHE HIT"s);

	// Finally the result
	switch(status.code())
	{
		using rocksdb::Status;

		case Status::kOk:          return true;
		case Status::kNotFound:    return false;
		default:
			throw_on_error(status);
			__builtin_unreachable();
	}
}

void
ircd::db::column::operator()(const delta &delta,
                             const sopts &sopts)
{
	operator()(&delta, &delta + 1, sopts);
}

void
ircd::db::column::operator()(const sopts &sopts,
                             const std::initializer_list<delta> &deltas)
{
	operator()(deltas, sopts);
}

void
ircd::db::column::operator()(const std::initializer_list<delta> &deltas,
                             const sopts &sopts)
{
	operator()(std::begin(deltas), std::end(deltas), sopts);
}

void
ircd::db::column::operator()(const delta *const &begin,
                             const delta *const &end,
                             const sopts &sopts)
{
	database &d(*this);

	rocksdb::WriteBatch batch;
	std::for_each(begin, end, [this, &batch]
	(const delta &delta)
	{
		append(batch, *this, delta);
	});

	auto opts(make_opts(sopts));
	log.debug("'%s' @%lu PUT %zu column deltas",
	          name(d),
	          sequence(d),
	          std::distance(begin, end));

	throw_on_error
	{
		d.d->Write(opts, &batch)
	};
}

void
ircd::db::column::operator()(const string_view &key,
                             const gopts &gopts,
                             const view_closure &func)
{
	return operator()(key, func, gopts);
}

void
ircd::db::column::operator()(const string_view &key,
                             const view_closure &func,
                             const gopts &gopts)
{
	const auto it(seek(*this, key, gopts));
	valid_equal_or_throw(*it, key);
	func(val(*it));
}

ircd::db::cell
ircd::db::column::operator[](const string_view &key)
const
{
	return { *this, key };
}


///////////////////////////////////////////////////////////////////////////////
//
// column::const_iterator
//

namespace ircd {
namespace db   {

} // namespace db
} // namespace ircd

ircd::db::column::const_iterator
ircd::db::column::end(const gopts &gopts)
{
	return cend(gopts);
}

ircd::db::column::const_iterator
ircd::db::column::begin(const gopts &gopts)
{
	return cbegin(gopts);
}

ircd::db::column::const_iterator
ircd::db::column::cend(const gopts &gopts)
{
	return {};
}

ircd::db::column::const_iterator
ircd::db::column::cbegin(const gopts &gopts)
{
	const_iterator ret
	{
		c, {}, gopts
	};

	seek(ret, pos::FRONT);
	return std::move(ret);
}

ircd::db::column::const_iterator
ircd::db::column::upper_bound(const string_view &key,
                              const gopts &gopts)
{
	auto it(lower_bound(key, gopts));
	if(it && it.it->key().compare(slice(key)) == 0)
		++it;

	return std::move(it);
}

ircd::db::column::const_iterator
ircd::db::column::find(const string_view &key,
                       const gopts &gopts)
{
	auto it(lower_bound(key, gopts));
	if(!it || it.it->key().compare(slice(key)) != 0)
		return cend(gopts);

	return it;
}

ircd::db::column::const_iterator
ircd::db::column::lower_bound(const string_view &key,
                              const gopts &gopts)
{
	const_iterator ret
	{
		c, {}, gopts
	};

	seek(ret, key);
	return std::move(ret);
}

ircd::db::column::const_iterator::const_iterator(const_iterator &&o)
noexcept
:opts{std::move(o.opts)}
,c{std::move(o.c)}
,it{std::move(o.it)}
,val{std::move(o.val)}
{
}

ircd::db::column::const_iterator &
ircd::db::column::const_iterator::operator=(const_iterator &&o)
noexcept
{
	opts = std::move(o.opts);
	c = std::move(o.c);
	it = std::move(o.it);
	val = std::move(o.val);
	return *this;
}

ircd::db::column::const_iterator::const_iterator()
{
}

ircd::db::column::const_iterator::const_iterator(std::shared_ptr<database::column> c,
                                                 std::unique_ptr<rocksdb::Iterator> &&it,
                                                 gopts opts)
:opts{std::move(opts)}
,c{std::move(c)}
,it{std::move(it)}
{
	//if(!has_opt(this->opts, get::READAHEAD))
	//	this->gopts.readahead_size = DEFAULT_READAHEAD;
}

ircd::db::column::const_iterator::~const_iterator()
noexcept
{
}

ircd::db::column::const_iterator &
ircd::db::column::const_iterator::operator--()
{
	seek(*this, pos::PREV);
	return *this;
}

ircd::db::column::const_iterator &
ircd::db::column::const_iterator::operator++()
{
	seek(*this, pos::NEXT);
	return *this;
}

const ircd::db::column::const_iterator::value_type &
ircd::db::column::const_iterator::operator*()
const
{
	assert(valid(*it));
	val.first = db::key(*it);
	val.second = db::val(*it);
	return val;
}

const ircd::db::column::const_iterator::value_type *
ircd::db::column::const_iterator::operator->()
const
{
	return &operator*();
}

bool
ircd::db::column::const_iterator::operator!()
const
{
	if(!it)
		return true;

	if(!valid(*it))
		return true;

	return false;
}

ircd::db::column::const_iterator::operator bool()
const
{
	return !!*this;
}

bool
ircd::db::operator!=(const column::const_iterator &a, const column::const_iterator &b)
{
	return !(a == b);
}

bool
ircd::db::operator==(const column::const_iterator &a, const column::const_iterator &b)
{
	if(a && b)
	{
		const auto &ak(a.it->key());
		const auto &bk(b.it->key());
		return ak.compare(bk) == 0;
	}

	if(!a && !b)
		return true;

	return false;
}

bool
ircd::db::operator>(const column::const_iterator &a, const column::const_iterator &b)
{
	if(a && b)
	{
		const auto &ak(a.it->key());
		const auto &bk(b.it->key());
		return ak.compare(bk) == 1;
	}

	if(!a && b)
		return true;

	if(!a && !b)
		return false;

	assert(!a && b);
	return false;
}

bool
ircd::db::operator<(const column::const_iterator &a, const column::const_iterator &b)
{
	if(a && b)
	{
		const auto &ak(a.it->key());
		const auto &bk(b.it->key());
		return ak.compare(bk) == -1;
	}

	if(!a && b)
		return false;

	if(!a && !b)
		return false;

	assert(a && !b);
	return true;
}

template<class pos>
bool
ircd::db::seek(column::const_iterator &it,
               const pos &p)
{
	database::column &c(it);
	const gopts &gopts(it);
	auto opts
	{
		make_opts(gopts, true)
	};

	return seek(c, p, opts, it.it);
}
template bool ircd::db::seek<ircd::db::pos>(column::const_iterator &, const pos &);
template bool ircd::db::seek<ircd::string_view>(column::const_iterator &, const string_view &);

///////////////////////////////////////////////////////////////////////////////
//
// seek
//

std::unique_ptr<rocksdb::Iterator>
ircd::db::seek(column &column,
               const string_view &key,
               const gopts &opts)
{
	database &d(column);
	database::column &c(column);

	std::unique_ptr<rocksdb::Iterator> ret;
	seek(c, key, opts, ret);
	return std::move(ret);
}

template<class pos>
bool
ircd::db::seek(database::column &c,
               const pos &p,
               const gopts &gopts,
               std::unique_ptr<rocksdb::Iterator> &it)
{
	auto opts
	{
		make_opts(gopts)
	};

	return seek(c, p, opts, it);
}

//
// Seek with offload-safety in case of blocking IO.
//
// The options for an iterator cannot be changed after the iterator is created.
// This slightly complicates our toggling between blocking and non-blocking queries.
//
template<class pos>
bool
ircd::db::seek(database::column &c,
               const pos &p,
               rocksdb::ReadOptions &opts,
               std::unique_ptr<rocksdb::Iterator> &it)
{
	database &d(*c.d);
	rocksdb::ColumnFamilyHandle *const &cf(c);

	// The ReadOptions created by make_opts(gopts) always sets NON_BLOCKING
	// mode. The user should never touch this. Only this function will ever
	// deal with iterators in BLOCKING mode.
	assert(opts.read_tier == NON_BLOCKING);

	if(!it)
		it.reset(d.d->NewIterator(opts, cf));

	// Start with a non-blocking query.
	_seek_(*it, p);

	// Indicate a cache miss and blocking is required.
	if(!it->status().IsIncomplete())
	{
		log.debug("'%s':'%s' @%lu SEEK valid[%d] CACHE HIT %s",
		          name(d),
		          name(c),
		          sequence(d),
		          valid(*it),
		          it->status().ToString());

		return valid(*it);
	}

	// DB cache miss: create a blocking iterator and offload it.
	rocksdb::ReadOptions blocking_opts(opts);
	blocking_opts.fill_cache = true;
	blocking_opts.read_tier = BLOCKING;
	std::unique_ptr<rocksdb::Iterator> blocking_it
	{
		d.d->NewIterator(blocking_opts, cf)
	};

	ctx::offload([&blocking_it, &it, &p]
	{
		// When the non-blocking iterator hit its cache miss in the middle of an
		// iteration we have to copy its position to the blocking iterator first
		// and then make the next query. TODO: this can be avoided when 'p' is a
		// slice and not an increment.
		if(valid(*it))
			_seek_(*blocking_it, it->key());

		if(!valid(*it) || valid(*blocking_it))
			_seek_(*blocking_it, p);
	});

	// When the blocking iterator comes back invalid the result is propagated
	if(!valid(*blocking_it))
	{
		it.reset(rocksdb::NewErrorIterator(blocking_it->status()));
		log.debug("'%s':'%s' @%lu SEEK valid[%d] CACHE MISS %s",
		          name(d),
		          name(c),
		          sequence(d),
		          valid(*it),
		          it->status().ToString());

		return false;
	}

	// When the blocking iterator comes back valid the result still has to be
	// properly transferred back to the user's non-blocking iterator. RocksDB
	// seems to be forcing us to recreate the iterator after it failed with the
	// status IsIncomplete(). Regardless of reuse, a non-blocking seek must occur
	// to match this iterator with the result -- such a seek may fail again if
	// the cache has been hosed between the point the offload took place and this
	// seek! To properly handle this case we reenter this seek() function and
	// enjoy the safety of offloading again for this edge case.
	it.reset(nullptr);
	log.debug("'%s':'%s' @%lu SEEK valid[%d] CACHE MISS %s",
	          name(d),
	          name(c),
	          sequence(d),
	          valid(*blocking_it),
	          blocking_it->status().ToString());

	return seek(c, blocking_it->key(), opts, it);
}

void
ircd::db::_seek_(rocksdb::Iterator &it,
                 const pos &p)
{
	switch(p)
	{
		case pos::NEXT:     it.Next();           break;
		case pos::PREV:     it.Prev();           break;
		case pos::FRONT:    it.SeekToFirst();    break;
		case pos::BACK:     it.SeekToLast();     break;
		default:
		case pos::END:
		{
			it.SeekToLast();
			if(it.Valid())
				it.Next();

			break;
		}
	}
}

void
ircd::db::_seek_(rocksdb::Iterator &it,
                 const string_view &sv)
{
	_seek_(it, slice(sv));
}

void
ircd::db::_seek_(rocksdb::Iterator &it,
                 const rocksdb::Slice &sk)
{
	it.Seek(sk);
}

///////////////////////////////////////////////////////////////////////////////
//
// Misc
//

std::vector<std::string>
ircd::db::column_names(const std::string &path,
                       const std::string &options)
{
	return column_names(path, database::options(options));
}

std::vector<std::string>
ircd::db::column_names(const std::string &path,
                       const rocksdb::DBOptions &opts)
try
{
	std::vector<std::string> ret;
	throw_on_error
	{
		rocksdb::DB::ListColumnFamilies(opts, path, &ret)
	};

	return ret;
}
catch(const io_error &e)
{
	return // No database found at path. Assume fresh.
	{
		{ rocksdb::kDefaultColumnFamilyName }
	};
}

ircd::db::database::options::options(const database &d)
:options{d.d->GetDBOptions()}
{
}

ircd::db::database::options::options(const database::column &c)
:options
{
	rocksdb::ColumnFamilyOptions
	{
		c.d->d->GetOptions(c.handle.get())
	}
}{}

ircd::db::database::options::options(const rocksdb::DBOptions &opts)
{
	throw_on_error
	{
		rocksdb::GetStringFromDBOptions(this, opts)
	};
}

ircd::db::database::options::options(const rocksdb::ColumnFamilyOptions &opts)
{
	throw_on_error
	{
		rocksdb::GetStringFromColumnFamilyOptions(this, opts)
	};
}

ircd::db::database::options::operator rocksdb::PlainTableOptions()
const
{
	rocksdb::PlainTableOptions ret;
	throw_on_error
	{
		rocksdb::GetPlainTableOptionsFromString(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::operator rocksdb::BlockBasedTableOptions()
const
{
	rocksdb::BlockBasedTableOptions ret;
	throw_on_error
	{
		rocksdb::GetBlockBasedTableOptionsFromString(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::operator rocksdb::ColumnFamilyOptions()
const
{
	rocksdb::ColumnFamilyOptions ret;
	throw_on_error
	{
		rocksdb::GetColumnFamilyOptionsFromString(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::operator rocksdb::DBOptions()
const
{
	rocksdb::DBOptions ret;
	throw_on_error
	{
		rocksdb::GetDBOptionsFromString(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::operator rocksdb::Options()
const
{
	rocksdb::Options ret;
	throw_on_error
	{
		rocksdb::GetOptionsFromString(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::map::map(const options &o)
{
	throw_on_error
	{
		rocksdb::StringToMap(o, this)
	};
}

ircd::db::database::options::map::operator rocksdb::PlainTableOptions()
const
{
	rocksdb::PlainTableOptions ret;
	throw_on_error
	{
		rocksdb::GetPlainTableOptionsFromMap(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::map::operator rocksdb::BlockBasedTableOptions()
const
{
	rocksdb::BlockBasedTableOptions ret;
	throw_on_error
	{
		rocksdb::GetBlockBasedTableOptionsFromMap(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::map::operator rocksdb::ColumnFamilyOptions()
const
{
	rocksdb::ColumnFamilyOptions ret;
	throw_on_error
	{
		rocksdb::GetColumnFamilyOptionsFromMap(ret, *this, &ret)
	};

	return ret;
}

ircd::db::database::options::map::operator rocksdb::DBOptions()
const
{
	rocksdb::DBOptions ret;
	throw_on_error
	{
		rocksdb::GetDBOptionsFromMap(ret, *this, &ret)
	};

	return ret;
}

bool
ircd::db::optstr_find_and_remove(std::string &optstr,
                                 const std::string &what)
{
	const auto pos(optstr.find(what));
	if(pos == std::string::npos)
		return false;

	optstr.erase(pos, what.size());
	return true;
}

rocksdb::ReadOptions
ircd::db::make_opts(const gopts &opts,
                    const bool &iterator)
{
	rocksdb::ReadOptions ret;
	ret.snapshot = opts.snapshot;
	ret.read_tier = NON_BLOCKING;
	//ret.total_order_seek = true;
	//ret.iterate_upper_bound = nullptr;

	if(iterator)
	{
		ret.fill_cache = false;
		ret.readahead_size = 4_KiB;
	}
	else ret.fill_cache = true;

	for(const auto &opt : opts) switch(opt.first)
	{
		case get::PIN:
			ret.pin_data = true;
			continue;

		case get::CACHE:
			ret.fill_cache = true;
			continue;

		case get::NO_CACHE:
			ret.fill_cache = false;
			continue;

		case get::NO_SNAPSHOT:
			ret.tailing = true;
			continue;

		case get::NO_CHECKSUM:
			ret.verify_checksums = false;
			continue;

		case get::READAHEAD:
			ret.readahead_size = opt.second;
			continue;

		default:
			continue;
	}

	return ret;
}

rocksdb::WriteOptions
ircd::db::make_opts(const sopts &opts)
{
	rocksdb::WriteOptions ret;
	for(const auto &opt : opts) switch(opt.first)
	{
		case set::FSYNC:
			ret.sync = true;
			continue;

		case set::NO_JOURNAL:
			ret.disableWAL = true;
			continue;

		case set::MISSING_COLUMNS:
			ret.ignore_missing_column_families = true;
			continue;

		default:
			continue;
	}

	return ret;
}

void
ircd::db::valid_equal_or_throw(const rocksdb::Iterator &it,
                               const string_view &sv)
{
	valid_or_throw(it);
	if(it.key().compare(slice(sv)) != 0)
		throw not_found();
}

bool
ircd::db::valid_equal(const rocksdb::Iterator &it,
                      const string_view &sv)
{
	if(!valid(it))
		return false;

	if(it.key().compare(slice(sv)) != 0)
		return false;

	return true;
}

void
ircd::db::valid_or_throw(const rocksdb::Iterator &it)
{
	if(!valid(it))
	{
		throw_on_error(it.status());
		throw not_found();
		//assert(0); // status == ok + !Valid() == ???
	}
}

bool
ircd::db::operator!(const rocksdb::Iterator &it)
{
	return !it.Valid();
}

bool
ircd::db::valid(const rocksdb::Iterator &it)
{
	return it.Valid();
}

ircd::db::throw_on_error::throw_on_error(const rocksdb::Status &s)
{
	using rocksdb::Status;

	switch(s.code())
	{
		case Status::kOk:                   return;
		case Status::kNotFound:             throw not_found("%s", s.ToString());
		case Status::kCorruption:           throw corruption("%s", s.ToString());
		case Status::kNotSupported:         throw not_supported("%s", s.ToString());
		case Status::kInvalidArgument:      throw invalid_argument("%s", s.ToString());
		case Status::kIOError:              throw io_error("%s", s.ToString());
		case Status::kMergeInProgress:      throw merge_in_progress("%s", s.ToString());
		case Status::kIncomplete:           throw incomplete("%s", s.ToString());
		case Status::kShutdownInProgress:   throw shutdown_in_progress("%s", s.ToString());
		case Status::kTimedOut:             throw timed_out("%s", s.ToString());
		case Status::kAborted:              throw aborted("%s", s.ToString());
		case Status::kBusy:                 throw busy("%s", s.ToString());
		case Status::kExpired:              throw expired("%s", s.ToString());
		case Status::kTryAgain:             throw try_again("%s", s.ToString());
		default:
			throw error("code[%d] %s", s.code(), s.ToString());
	}
}

std::vector<std::string>
ircd::db::available()
{
	const auto prefix(fs::get(fs::DB));
	const auto dirs(fs::ls(prefix));
	return dirs;
}

std::string
ircd::db::path(const std::string &name)
{
	const auto prefix(fs::get(fs::DB));
	return fs::make_path({prefix, name});
}

std::pair<ircd::string_view, ircd::string_view>
ircd::db::operator*(const rocksdb::Iterator &it)
{
	return { key(it), val(it) };
}

ircd::string_view
ircd::db::key(const rocksdb::Iterator &it)
{
	return slice(it.key());
}

ircd::string_view
ircd::db::val(const rocksdb::Iterator &it)
{
	return slice(it.value());
}

rocksdb::Slice
ircd::db::slice(const string_view &sv)
{
	return { sv.data(), sv.size() };
}

ircd::string_view
ircd::db::slice(const rocksdb::Slice &sk)
{
	return { sk.data(), sk.size() };
}

const std::string &
ircd::db::reflect(const rocksdb::Tickers &type)
{
	const auto &names(rocksdb::TickersNameMap);
	const auto it(std::find_if(begin(names), end(names), [&type]
	(const auto &pair)
	{
		return pair.first == type;
	}));

	static const auto empty{"<ticker>?????"s};
	return it != end(names)? it->second : empty;
}

const std::string &
ircd::db::reflect(const rocksdb::Histograms &type)
{
	const auto &names(rocksdb::HistogramsNameMap);
	const auto it(std::find_if(begin(names), end(names), [&type]
	(const auto &pair)
	{
		return pair.first == type;
	}));

	static const auto empty{"<histogram>?????"s};
	return it != end(names)? it->second : empty;
}

const char *
ircd::db::reflect(const pos &pos)
{
	switch(pos)
	{
		case pos::NEXT:     return "NEXT";
		case pos::PREV:     return "PREV";
		case pos::FRONT:    return "FRONT";
		case pos::BACK:     return "BACK";
		case pos::END:      return "END";
	}

	return "?????";
}

bool
ircd::db::value_required(const op &op)
{
	switch(op)
	{
		case op::SET:
		case op::MERGE:
		case op::DELETE_RANGE:
			return true;

		case op::GET:
		case op::DELETE:
		case op::SINGLE_DELETE:
			return false;
	}
}
