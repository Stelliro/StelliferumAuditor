
#ifndef AUDITOR_H
#define AUDITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>  /* size_t — required on musl/Alpine (not implied by stdint.h) */
#include <stdbool.h>

// Forward declaration (full definition in web_lookup.h)
struct WebLookupState;

#define MAX_NAME_LEN 128
#define MAX_CLASSNAME_LEN 128
#define MAX_CATEGORY_LEN 64
#define MAX_USAGE_LEN 64
#define MAX_VALUE_LEN 64
#define MAX_PATH_LEN 512
#define MAX_ITEMS 65000 
#define MAX_TIERS 16   // array capacity; runtime tier count comes from loot_policy (lp_tier_count)
#define MAX_USAGES_PER_TIER 32
#define MAX_VALUES_PER_ITEM 16
#define MAX_ISSUES 30000
#define MAX_ISSUE_MSG_LEN 256
#define MAX_INPUT_FILES 256
#define MAX_SPAWNABLE_BLOCK_LEN 4096
#define MAX_SPAWNABLE_BLOCKS 8192

// Classname hash map for O(1) inline dedup during parsing.
// Power-of-2 size so we can use & instead of % for modular indexing.
#define CLASSNAME_MAP_BUCKETS 131072   // 2^17 — ~2x MAX_ITEMS for low collision rate
#define CLASSNAME_MAP_EMPTY   (-1)     // Sentinel: bucket is unused

// Persistent file fingerprint cache — tracks file path + size so unchanged
// files are skipped entirely on subsequent loads.
#define FILE_CACHE_MAX_ENTRIES 2048
#define FILE_CACHE_PATH "config/file_cache.csv"

// Integrity engine limits
#define MAX_INTEGRITY_FINDINGS 128

// File index limits
#define MAX_INDEX_ENTRIES 4096
#define MAX_SWARM_TASKS 256
#define MAX_TERRITORY_FILES 128
#define MAX_STITCH_GROUPS 64

#define FLAG_CARGO 1
#define FLAG_HOARDER 2
#define FLAG_MAP 4
#define FLAG_PLAYER 8
#define FLAG_CRAFTED 16
#define FLAG_DELOOT 32

// Phoenix item capacity system
#define PHOENIX_MAX_NO_CARGO      512
#define PHOENIX_MAX_SPECIFIC      128
#define PHOENIX_MAX_MODDED_SLOTS  128
#define PHOENIX_MAX_HAS_CARGO     256
#define PHOENIX_MAX_CARGO_ITEMS   8
#define PHOENIX_MAX_PATTERNS      32

typedef enum { ZONE_CIVILIAN, ZONE_MILITARY, ZONE_ENDGAME, ZONE_UNKNOWN } Zone;
typedef enum { ISSUE_NONE, ISSUE_ORPHAN, ISSUE_WRONG_ZONE, ISSUE_NOMINAL_HIGH, ISSUE_NOMINAL_LOW, ISSUE_FORBIDDEN_CAT, ISSUE_MISSING_USAGE, ISSUE_DUPLICATE, ISSUE_LIFETIME_SHORT, ISSUE_LIFETIME_LONG } IssueType;
typedef enum { SEVERITY_INFO, SEVERITY_WARNING, SEVERITY_ERROR, SEVERITY_CRITICAL } Severity;
typedef enum { STATE_IDLE, STATE_ANALYZING, STATE_SUCCESS, STATE_FAILED } AuditorState;

// Shop/Trader mod system — controls which format the trader export generates
typedef enum {
    SHOP_MOD_TRADERPLUS,   // TraderPlus JSON (TraderPlusTrading.json)
    SHOP_MOD_DRJONES,      // Dr. Jones Trader TXT (TraderConfig.txt)
    SHOP_MOD_EXPANSION,    // Expansion Market JSON (future)
    SHOP_MOD_COUNT         // sentinel — total number of shop mods
} ShopMod;

// File types
typedef enum { FILE_TYPE_UNKNOWN, FILE_TYPE_ECONOMY, FILE_TYPE_SPAWNABLE, FILE_TYPE_TERRITORY, FILE_TYPE_GLOBALS, FILE_TYPE_CONFIG, FILE_TYPE_EVENTS, FILE_TYPE_TRADER, FILE_TYPE_RANDOMPRESETS } FileType;

typedef struct { Severity severity; char message[MAX_ISSUE_MSG_LEN]; bool resolved; } AuditIssue;

typedef struct {
    char classname[MAX_CLASSNAME_LEN];
    int nominal; int lifetime; int restock; int min; int quantmin; int quantmax; int cost;
    char category[MAX_CATEGORY_LEN];
    char usages[MAX_USAGES_PER_TIER][MAX_USAGE_LEN]; int usage_count;
    char values[MAX_VALUES_PER_ITEM][MAX_VALUE_LEN]; int value_count;
    int flags; 
    int assigned_tier; int calculated_price; char trader_cat[64];
    int buy_price; int sell_price; int stock_override; int restock_override;
    char currency[64]; bool black_market; bool admin_only;
    char mod_source[64]; bool deleted; bool modified;
    char mod_name[128];   // e.g. "@SNAFU Weapons", "vanilla", "server_root"
    bool is_debug_item;   // true if classname matches debug/test/dev patterns
    bool renamed;         // true if classname was prefixed to avoid collision
    FileType file_type;
} LootItem;

typedef struct { char classname[MAX_CLASSNAME_LEN]; int tier; } KnownItem;
typedef struct { char classname[MAX_CLASSNAME_LEN]; char source_file[64]; char raw_xml[MAX_SPAWNABLE_BLOCK_LEN]; } SpawnableBlock;
typedef struct { int total_items; int total_issues; int items_by_tier[MAX_TIERS + 1]; int issues_by_severity[4]; float score; bool pass; } AuditSummary;

// ============================================================================
// FILE INDEX: Tracks every downloaded file's path, type, mod origin, and map
// ============================================================================
typedef enum { MAP_UNKNOWN, MAP_CHERNARUSPLUS, MAP_ENOCH, MAP_SAKHAL } MapId;

typedef struct {
    char filepath[MAX_PATH_LEN];
    char filename[128];
    char mod_name[128];       // e.g. "@SNAFU Weapons", "vanilla", "server_root"
    FileType file_type;
    MapId map_id;
    bool is_vanilla;          // true if from mpmissions/ (base game files)
    bool is_mod;              // true if from @ModName/ directory
    bool processed;
} FileIndexEntry;

typedef struct {
    FileIndexEntry entries[MAX_INDEX_ENTRIES];
    int count;
} FileIndex;

// ============================================================================
// STITCH GROUP: Groups files of the same type for intelligent merging
// ============================================================================
typedef struct {
    char group_name[128];     // e.g. "bear_territories", "types", "cfgspawnabletypes"
    FileType file_type;
    MapId map_id;
    char vanilla_path[MAX_PATH_LEN];   // The base/vanilla file
    char mod_paths[32][MAX_PATH_LEN];  // Mod files to merge into vanilla
    int mod_count;
    char output_path[MAX_PATH_LEN];    // Where the merged result goes
} StitchGroup;

// ============================================================================
// REMOTE FILE BROWSER
// ============================================================================
#define MAX_BROWSER_ENTRIES 2048

typedef struct {
    char name[256];
    char full_path[MAX_PATH_LEN];
    bool is_directory;
    long long size;
    char date_str[32];          // e.g. "02-01-26 04:21PM"
} RemoteFileEntry;

typedef struct {
    RemoteFileEntry entries[MAX_BROWSER_ENTRIES];
    int count;
    char current_path[MAX_PATH_LEN];   // Current remote directory
    char error[256];
    volatile bool loading;             // True while WinSCP is running
    volatile bool ready;               // True when results are ready to display
    int  selected;                     // Currently selected entry index
} RemoteFileBrowser;

// ============================================================================
// PHOENIX: AI-driven item capacity classification
// ============================================================================
// Loaded from .phoenix/item_capacity.json at gap-fill time.
// Replaces hardcoded is_no_cargo_clothing() with dynamic lookup.

typedef struct {
    char classname[MAX_CLASSNAME_LEN];
    char cargo_items[PHOENIX_MAX_CARGO_ITEMS][MAX_CLASSNAME_LEN];
    int  cargo_item_count;
    float cargo_chance;
} PhoenixSpecificItem;

typedef struct {
    // Items with NO cargo (array of classnames)
    char no_cargo[PHOENIX_MAX_NO_CARGO][MAX_CLASSNAME_LEN];
    int  no_cargo_count;

    // Items with only specific cargo
    PhoenixSpecificItem specific[PHOENIX_MAX_SPECIFIC];
    int specific_count;

    // Items confirmed to have cargo (array of classnames)
    char has_cargo[PHOENIX_MAX_HAS_CARGO][MAX_CLASSNAME_LEN];
    int  has_cargo_count;

    // Fallback patterns (case-insensitive substrings)
    char no_cargo_patterns[PHOENIX_MAX_PATTERNS][MAX_CLASSNAME_LEN];
    int  no_cargo_pattern_count;
    char boot_patterns[PHOENIX_MAX_PATTERNS][MAX_CLASSNAME_LEN];
    int  boot_pattern_count;
    char hat_patterns[PHOENIX_MAX_PATTERNS][MAX_CLASSNAME_LEN];
    int  hat_pattern_count;

    bool loaded;  // true after successful config parse
} PhoenixCapacity;

// ============================================================================
// INTEGRITY: Pre-audit and post-sew verification
// ============================================================================
typedef struct {
    Severity severity;
    char message[MAX_ISSUE_MSG_LEN];
    char classname[MAX_CLASSNAME_LEN];
    bool is_variation;       // true if this is a suspicious classname variation
    bool is_deleted_leak;    // true if a deleted item leaked into output
} IntegrityFinding;

typedef struct {
    bool passed;                                       // true if all checks passed
    char phase[32];                                    // "PRE_AUDIT" or "SEW_INTEGRITY"
    char summary[512];                                 // Human-readable summary
    IntegrityFinding findings[MAX_INTEGRITY_FINDINGS]; // Detailed findings
    int finding_count;
    int total_items;         // Total items examined
    int active_items;        // Non-deleted items (or actual output count for sew)
    int duplicate_groups;    // Number of classname collision groups
} IntegrityReport;

// ============================================================================
// SWARM: Agent pipeline system
// ============================================================================
typedef enum {
    AGENT_INDEX,       // Primary: Scan and classify all files
    AGENT_SORT,        // Primary: Sort downloaded files by content into organized dirs
    AGENT_PARSE,       // Primary: Parse all economy files
    AGENT_AUDIT,       // Primary: Validate and tier-assign
    AGENT_STITCH,      // Primary: Merge file groups
    AGENT_EXPORT,      // Primary: Write output files
    AGENT_RELAY        // Communication relay between agents
} AgentType;

typedef enum {
    TASK_PENDING,
    TASK_IN_PROGRESS,
    TASK_COMPLETE,
    TASK_FAILED
} TaskStatus;

typedef struct {
    int id;
    AgentType agent;
    TaskStatus status;
    char description[256];
    char result[512];
    int items_processed;
    int errors;
} SwarmTask;

typedef struct {
    SwarmTask tasks[MAX_SWARM_TASKS];
    int task_count;
    int current_task;
    int primary_agent_count;    // Determined by file/token count
    int total_files;
    int total_tokens;           // Estimated token count for LLM context
    bool complete;
} SwarmState;

// ============================================================================
// CLASSNAME HASH MAP — O(1) inline dedup during item loading
// ============================================================================
// Open-addressing hash table mapping lowercase classname → item index.
// When parser_load_types_xml() encounters a classname that already exists,
// it merges into the existing item instead of appending a duplicate.
typedef struct {
    int buckets[CLASSNAME_MAP_BUCKETS];   // Each is CLASSNAME_MAP_EMPTY or index into items[]
} ClassnameMap;

// ============================================================================
// FILE FINGERPRINT CACHE — skip unchanged files across runs
// ============================================================================
// Persisted to FILE_CACHE_PATH (CSV). On load, files whose path+size match
// the cache are skipped entirely. Files that no longer exist on disk are
// pruned from the cache automatically.
typedef struct {
    char   path[MAX_PATH_LEN];
    long   size;               // File size in bytes at time of last load
    int    item_count;         // How many items this file contributed
} FileCacheEntry;

typedef struct {
    FileCacheEntry entries[FILE_CACHE_MAX_ENTRIES];
    int count;
    bool loaded;               // True once cache has been read from disk
} FileCache;

typedef struct {
    AuditorState state; char status_message[256];
    LootItem items[MAX_ITEMS]; int item_count;
    AuditIssue issues[MAX_ISSUES]; int issue_count;
    KnownItem known_items[MAX_ITEMS]; int known_item_count;
    SpawnableBlock spawn_blocks[MAX_SPAWNABLE_BLOCKS]; int spawn_block_count;
    AuditSummary summary;
    char output_dir[MAX_PATH_LEN]; char config_dir[MAX_PATH_LEN]; char backup_dir[MAX_PATH_LEN];
    int tier_rule_count; int selected_item;
    // New subsystems
    FileIndex file_index;
    StitchGroup stitch_groups[MAX_STITCH_GROUPS]; int stitch_group_count;
    SwarmState swarm;
    RemoteFileBrowser browser;
    // File sorter results
    int sorted_types_count;
    int sorted_spawnable_count;
    int sorted_trader_count;
    int sorted_territory_count;
    int sorted_events_count;
    int sorted_globals_count;
    int sorted_config_count;
    int sorted_randompresets_count;
    int sorted_unknown_count;
    int sorted_total_count;
    bool files_sorted;             // True once file_sorter_run() has completed
    // Trader items loaded from existing trader files
    int trader_classname_count;
    char trader_classnames[MAX_ITEMS][MAX_CLASSNAME_LEN]; // Classnames already in trader
    // Snapshot of original items for diff view
    LootItem *original_items;      // Heap-allocated snapshot (NULL until first load)
    int original_item_count;
    bool data_loaded;              // True once data has been loaded (prevents re-parsing in swarm)
    bool audit_complete;            // True once audit pipeline (or swarm) has run — prevents sew from re-running destructive passes
    // Web lookup (heap-allocated, NULL until first use)
    struct WebLookupState *web;    // Internet-based mod attachment discovery
    // Phoenix capacity rules (loaded from .phoenix/item_capacity.json)
    PhoenixCapacity phoenix;
    // Integrity engine reports (populated by audit pipeline)
    IntegrityReport pre_audit_integrity;
    IntegrityReport sew_integrity;
    // Shop/Trader mod selection
    ShopMod shop_mod;              // Which trader format to export (default: SHOP_MOD_TRADERPLUS)
    // Inline dedup: classname → item index hash map (heap-allocated, NULL until first load)
    ClassnameMap *classname_map;
    int dedup_skipped;             // Count of items merged inline (for logging)
    // Persistent file fingerprint cache
    FileCache file_cache;
} AuditorContext;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Parser
bool parser_load_types_xml(AuditorContext *ctx, const char *filepath);
bool parser_load_known_items(AuditorContext *ctx, const char *filepath);
FileType parser_detect_file_type(const char *filepath);
bool parser_load_spawnable_xml(AuditorContext *ctx, const char *filepath);

// Loot
int loot_get_mod_tier(LootItem *item);

// Auditor
void auditor_run_full_audit(AuditorContext *ctx);
void auditor_resolve_duplicates(AuditorContext *ctx);
void auditor_sort_items(AuditorContext *ctx);
void auditor_assign_tier(AuditorContext *ctx, LootItem *item);
void auditor_validate_item(AuditorContext *ctx, int index);
void auditor_run_swarm(AuditorContext *ctx);
void auditor_fill_missing_data(AuditorContext *ctx);
void auditor_sanitize_ce_values(AuditorContext *ctx);  // Clamp value tags to CE-compatible Tier1-4
void auditor_rebalance_usage_pools(AuditorContext *ctx); // Diversify usage flags by category
void auditor_generate_store_data(AuditorContext *ctx);
void auditor_snapshot_originals(AuditorContext *ctx);
void auditor_filter_debug_items(AuditorContext *ctx);
void auditor_rename_collisions(AuditorContext *ctx);

// Integrity Engine
IntegrityReport auditor_pre_audit_integrity(AuditorContext *ctx);
IntegrityReport auditor_sew_integrity_check(AuditorContext *ctx,
                                             const char *types_path,
                                             const char *spawnables_path);
void auditor_run_audit_pipeline(AuditorContext *ctx);

// Pre-upload output validation — verifies all output files are safe for server.
// Returns true if ALL files pass validation. Sets out_passed/out_failed counts.
// Checks: XML well-formedness, mandatory sections, no Tier5+, no duplicate types.
bool auditor_validate_output_for_upload(int *out_passed, int *out_failed);

// Remediate-and-validate loop — runs validation, auto-fixes remediable issues
// (duplicate types, Tier5+ values) by re-exporting affected files, then
// re-validates.  Up to 3 passes.  Only fails on non-recoverable corruption.
bool auditor_remediate_and_validate(AuditorContext *ctx, int *out_passed, int *out_failed);

// Gap Filler
void gap_fill_missing_data(AuditorContext *ctx);
void gap_fill_spawnable_types(AuditorContext *ctx);

// Phoenix — AI-driven item capacity discovery
void phoenix_load_capacity(AuditorContext *ctx);
void run_phoenix_scanner(AuditorContext *ctx);

// Web Lookup (see web_lookup.h for full API)
void web_lookup_ensure_ready(AuditorContext *ctx);

// File Sorter — classifies and organizes downloaded files by content
void file_sorter_run(AuditorContext *ctx, const char *download_root, const char *sorted_root);
void file_sorter_print_summary(AuditorContext *ctx, const char *sorted_root);

// Trader Gap Filler — creates TraderPlus entries for items missing from trader files
void trader_gap_fill(AuditorContext *ctx, const char *sorted_root);
bool parser_load_traderplus_json(AuditorContext *ctx, const char *filepath);

// Writer
bool writer_export_merged_xml(AuditorContext *ctx, const char *filepath);
bool writer_export_spawnable_types(AuditorContext *ctx, const char *filepath);
bool writer_export_trader_config(AuditorContext *ctx, const char *filepath);
bool writer_export_drjones_config(AuditorContext *ctx, const char *filepath);
bool writer_export_trader_by_shop_mod(AuditorContext *ctx);  // Dispatches to correct format
bool writer_export_trader_shops(AuditorContext *ctx, const char *output_dir); // Per-shop files (output/shops/)
bool writer_export_csv(AuditorContext *ctx, const char *filepath);

// Shop mod helpers
const char* shop_mod_name(ShopMod mod);
const char* shop_mod_output_filename(ShopMod mod);
const char* shop_mod_default_remote_path(ShopMod mod);
bool writer_export_audit_report(AuditorContext *ctx, const char *filepath, bool include_duplicates);
bool writer_export_zombie_config(AuditorContext *ctx, const char *output_dir);
bool writer_export_spawn_templates(AuditorContext *ctx, const char *output_dir);
bool writer_export_cfgeconomycore(AuditorContext *ctx, const char *filepath);
bool writer_export_cfglimitsdefinitionuser(AuditorContext *ctx, const char *filepath);
bool writer_merge_random_presets(AuditorContext *ctx, const char *sorted_root, const char *output_path);
bool writer_create_backup(const char *filepath, const char *backup_dir);

// SFL Generator (Search For Loot Improved)
bool sfl_generate_config(AuditorContext *ctx, const char *template_path, const char *output_path);

// File Index
void file_index_init(FileIndex *idx);
void file_index_scan(FileIndex *idx, const char *root_dir);
void file_index_classify(FileIndex *idx);
int  file_index_count_by_type(FileIndex *idx, FileType type);
int  file_index_count_by_mod(FileIndex *idx, const char *mod_name);
void file_index_print_summary(FileIndex *idx);
FileIndexEntry* file_index_find(FileIndex *idx, const char *filename, FileType type);
MapId file_index_detect_map(const char *filepath);

// Classname Map — inline dedup during item loading
void classname_map_init(AuditorContext *ctx);        // Allocate + clear
void classname_map_clear(AuditorContext *ctx);        // Reset all buckets to EMPTY (keeps allocation)
void classname_map_free(AuditorContext *ctx);         // Free heap memory
int  classname_map_find(AuditorContext *ctx, const char *classname);   // Returns item index or -1
void classname_map_insert(AuditorContext *ctx, const char *classname, int item_index);

// File Fingerprint Cache — skip unchanged files across runs
void file_cache_load(FileCache *cache);               // Load from FILE_CACHE_PATH
void file_cache_save(const FileCache *cache);          // Save to FILE_CACHE_PATH
void file_cache_prune(FileCache *cache);               // Remove entries for files that no longer exist
bool file_cache_is_unchanged(const FileCache *cache, const char *filepath, long file_size);
void file_cache_update(FileCache *cache, const char *filepath, long file_size, int item_count);

// Stitcher
void stitcher_init(AuditorContext *ctx);
void stitcher_build_groups(AuditorContext *ctx);
void stitcher_merge_territories(AuditorContext *ctx, const char *output_dir);
void stitcher_merge_spawnables(AuditorContext *ctx, const char *output_dir);
void stitcher_merge_all(AuditorContext *ctx, const char *output_dir);
bool stitcher_merge_xml_file(const char *base_path, const char *overlay_path, const char *output_path, const char *root_tag);

// Swarm
void swarm_init(SwarmState *swarm);
void swarm_plan(AuditorContext *ctx);
void swarm_execute(AuditorContext *ctx);
void swarm_add_task(SwarmState *swarm, AgentType agent, const char *description);
const char* swarm_agent_name(AgentType agent);
const char* swarm_task_status_name(TaskStatus status);

// Util – Log ring buffer (readable by UI)
#define LOG_RING_CAPACITY 4096
#define LOG_ENTRY_MAX     512

typedef struct {
    Severity severity;
    char     timestamp[16];   // "HH:MM:SS"
    char     message[LOG_ENTRY_MAX];
} LogEntry;

int          util_log_get_count(void);
const LogEntry* util_log_get_entry(int index);   // 0 = oldest visible

// Util – Child-process lifecycle (Win32 Job Object)
void util_init_job_object(void);                   // create job with KILL_ON_JOB_CLOSE
void util_assign_child_process(void *hProcess);    // add a child to the job
void util_close_job_object(void);                  // close job → kill all children

// Util
void util_init_context(AuditorContext *ctx);
void util_init_logger(void);
void util_close_logger(void);
void util_setup_console(void);                     // CLI/headless: attach parent console if any
void util_setup_console_quiet(void);               // CLI: no new window, no focus steal (log file only if no parent)
void util_log(Severity level, const char *fmt, ...);
int util_strcasecmp(const char *a, const char *b);
int util_strnicmp(const char *a, const char *b, size_t n); /* portable _strnicmp / strncasecmp */
bool util_str_contains_ci(const char *haystack, const char *needle);
void util_trim(char *str);
void util_timestamp(char *buf, size_t len);
unsigned int util_hash_string(const char *str);
bool util_file_exists(const char *path);
bool util_read_ini_value(const char *path, const char *key, char *out, size_t out_len);
const char* util_basename(const char *path);
bool util_find_file_by_name(const char *root, const char *filename, char *out, size_t out_len);
void util_index_touch(const char *filepath, FileType type);
bool util_index_is_stale(const char *filepath, int days);
void util_snapshot_base_files(const char *source_root, const char *base_root);
void util_generate_mod_manifest(const char *download_root, const char *output_path);
void util_ensure_directory(const char *path);
Zone util_tier_to_zone(int tier);

// Upload Daemon — background thread service for Save & Upload
typedef enum {
    UPLOAD_PHASE_GENERATING,     // Generating output files
    UPLOAD_PHASE_RESTORE_POINT,  // Creating pre-upload restore point
    UPLOAD_PHASE_UPLOADING,      // Uploading files to server
    UPLOAD_PHASE_DONE            // Complete (check result)
} UploadPhase;

typedef enum {
    UPLOAD_RESULT_SUCCESS,       // All files uploaded
    UPLOAD_RESULT_PARTIAL,       // Some files failed
    UPLOAD_RESULT_FAILED,        // All uploads failed or no files
    UPLOAD_RESULT_CANCELLED      // User cancelled
} UploadResult;

// Job descriptor — filled by UI, consumed by daemon thread
typedef struct {
    AuditorContext *ctx;
    ShopMod         shop_mod;
    char            host[128];
    char            user[128];
    char            pass[128];
    int             port;
    char            remote_root[256];
    char            remote_types[256];
    char            remote_trader[256];
} UploadDaemonJob;

// Read-only state polled by UI (written only by daemon thread)
typedef struct {
    // Progress
    volatile bool        running;           // Thread is alive
    volatile bool        finished;          // Thread has exited
    volatile bool        cancel_requested;  // UI sets this to request abort
    volatile UploadPhase phase;
    volatile UploadResult result;
    volatile float       progress_pct;      // 0.0–1.0 during upload phase
    volatile int         files_total;
    volatile int         files_uploaded;
    volatile int         files_failed;
    char                 phase_label[128];   // Human-readable current phase
    char                 current_file[256];  // File being processed
    char                 result_message[256]; // Final status message
} UploadDaemonState;

bool                     upload_daemon_start(UploadDaemonJob *job);
void                     upload_daemon_cancel(void);
const UploadDaemonState* upload_daemon_state(void);
bool                     upload_daemon_is_running(void);
void                     upload_daemon_cleanup(void);

// FTP
bool ftp_download_file(const char *host, int port, const char *user, const char *pass, const char *remote_path, const char *local_path);
bool ftp_upload_file(const char *host, int port, const char *user, const char *pass, const char *local_path, const char *remote_path);
bool ftp_upload_batch(const char *host, int port, const char *user, const char *pass,
                      const char **local_paths, const char **remote_paths, int count,
                      int *out_succeeded, int *out_failed);
bool ftp_cleanup_remote_dir(const char *host, int port, const char *user, const char *pass,
                            const char *remote_dir);
void ftp_set_cancel_flag(volatile bool *flag);
void ftp_set_upload_counter(volatile int *counter);
bool ftp_download_recursive(const char *host, int port, const char *user, const char *pass, const char *remote_dir, const char *local_dir);
bool ftp_download_batch(const char *host, int port, const char *user, const char *pass, const char **remote_paths, const char **local_paths, int count);
bool ftp_download_core_files(const char *host, int port, const char *user, const char *pass, const char *remote_root, const char *local_root);
bool ftp_upload_directory(const char *host, int port, const char *user, const char *pass, const char *local_dir, const char *remote_dir);
bool ftp_list_directory(const char *host, int port, const char *user, const char *pass, const char *remote_path, RemoteFileBrowser *browser);

// Native libcurl backend mirrors (src/ftp_native.c). Same signatures as public
// ftp_* core ops; dispatch track routes entry points here when STELLI_USE_LIBCURL.
// Credentials are never placed on process argv or plaintext temp scripts.
// SFTP host-key (config/ftp.ini): KNOWN_HOSTS, HOST_KEY_PIN, HOST_KEY_POLICY
// (pin|fail|trust) — first-connect pin defaults to config/known_hosts.
int  ftp_native_backend_available(void);
int  ftp_native_protocols_ok(void);
const char *ftp_native_curl_version_string(void);
void ftp_native_set_cancel_flag(volatile bool *flag);
void ftp_native_set_upload_counter(volatile int *counter);
/** Re-read host-key / known_hosts settings from config/ftp.ini (native path). */
void ftp_native_reload_security_config(void);
bool ftp_native_download_file(const char *host, int port, const char *user, const char *pass,
                              const char *remote_path, const char *local_path);
bool ftp_native_upload_file(const char *host, int port, const char *user, const char *pass,
                            const char *local_path, const char *remote_path);
bool ftp_native_upload_batch(const char *host, int port, const char *user, const char *pass,
                             const char **local_paths, const char **remote_paths, int count,
                             int *out_succeeded, int *out_failed);
bool ftp_native_download_batch(const char *host, int port, const char *user, const char *pass,
                               const char **remote_paths, const char **local_paths, int count);
bool ftp_native_list_directory(const char *host, int port, const char *user, const char *pass,
                               const char *remote_path, RemoteFileBrowser *browser);
/* Advanced native mirrors (ftp-native-advanced) — no WinSCP log scraping. */
bool ftp_native_download_recursive(const char *host, int port, const char *user, const char *pass,
                                   const char *remote_dir, const char *local_dir);
bool ftp_native_download_core_files(const char *host, int port, const char *user, const char *pass,
                                    const char *remote_root, const char *local_root);
bool ftp_native_upload_directory(const char *host, int port, const char *user, const char *pass,
                                 const char *local_dir, const char *remote_dir);
bool ftp_native_cleanup_remote_dir(const char *host, int port, const char *user, const char *pass,
                                   const char *remote_dir);
bool ftp_native_verify_uploads(const char *host, int port, const char *user, const char *pass,
                               const char **local_paths, const char **remote_paths, int count);
bool ftp_native_create_restore_point(const char *host, int port, const char *user, const char *pass,
                                     const char **remote_paths, int count,
                                     const char *backup_dir, const char *manifest_path);
bool ftp_native_restore_from_manifest(const char *host, int port, const char *user, const char *pass,
                                      const char *manifest_path);
bool ftp_create_restore_point(const char *host, int port, const char *user, const char *pass,
                              const char **remote_paths, int count,
                              const char *backup_dir, const char *manifest_path);
bool ftp_restore_from_manifest(const char *host, int port, const char *user, const char *pass,
                               const char *manifest_path);

// Restore point management — multiple timestamped restore points
#define MAX_RESTORE_POINTS 32

typedef struct {
    char label[64];              // Human-readable label ("2025-01-15 14:30:22")
    char dir[MAX_PATH_LEN];     // Directory path ("backups/20250115_143022")
    char manifest[MAX_PATH_LEN]; // Manifest path ("backups/20250115_143022/restore_manifest.txt")
} RestorePointInfo;

// Scan backups/ for directories containing restore_manifest.txt.
// Returns count of found restore points (newest first). Includes legacy last_upload/ if present.
int ftp_list_restore_points(RestorePointInfo *out, int max_count);

// Delete old restore points, keeping only the newest max_keep entries.
void ftp_purge_old_restore_points(int max_keep);

// Generate a timestamped backup directory name (e.g., "backups/20250115_143022").
void ftp_generate_restore_dir(char *dir_buf, int dir_size, char *manifest_buf, int manifest_size);

// Post-upload integrity check: verify uploaded files exist on server with correct sizes.
// Returns true if ALL files verified OK. Logs warnings for mismatches.
bool ftp_verify_uploads(const char *host, int port, const char *user, const char *pass,
                        const char **local_paths, const char **remote_paths, int count);

// Output directory backup & cleanup
// Compresses output/ to output_backups/YYYYMMDD_HHMMSS.zip, then deletes all files
// from output/ while preserving directory structure. Auto-purges old backups (keeps 10).
bool util_backup_and_clean_output(void);

// Stale file quarantine
// Scans output/ for files that don't match the expected pipeline outputs for the
// current SHOP_MOD configuration. Moves them to output_quarantine/YYYYMMDD_HHMMSS/.
// Returns the number of files quarantined.
int util_quarantine_stale_output(ShopMod shop_mod);

// UI
void ui_init(void);
void ui_run(AuditorContext *ctx);
void ui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
