/**
 * ftp_native.c — Native FTP/SFTP backend (libcurl)
 * =================================================
 * Implements core + advanced transfer ops used by the dispatch track:
 *   Core: download/upload single + batch, list directory, cancel + upload counter.
 *   Advanced: recursive/core download, upload_directory, cleanup, verify,
 *             create_restore_point, restore_from_manifest (no WinSCP log scraping).
 *
 * Public ftp_* entry points in ftp_manager.c prefer this module when
 * STELLI_USE_LIBCURL is defined (ftp_native_backend_available). WinSCP remains
 * an optional Windows-only fallback when native is not linked.
 *
 * Security (native path):
 *   - Credentials only via CURLOPT_USERNAME / CURLOPT_PASSWORD (in-process).
 *   - Never put password on process argv (no child process for transfers).
 *   - Never write plaintext password temp scripts (no .TEMP session scripts).
 *   - SFTP host-key verification via config/ftp.ini:
 *       KNOWN_HOSTS   — OpenSSH-style known_hosts path (default config/known_hosts)
 *       HOST_KEY_PIN  — optional SHA256 fingerprint pin (base64, optional "SHA256:" prefix)
 *       HOST_KEY_POLICY — pin (default first-connect write) | fail (strict) | trust (insecure)
 *
 * Protocol heuristic (match foundation): ports 22 / 2222 / 8827 => sftp://
 * else ftp://.
 *
 * Semantic note vs WinSCP synchronize -criteria=size:
 *   Native recursive uses remote LIST size + local file size equality to skip
 *   unchanged files (same intent; not WinSCP filemask engine). Exclude dir/ext
 *   lists match the foundation masks used by ftp_manager WinSCP path.
 */

#include "auditor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* Implemented in util.c; not always listed in public header. */
void util_trim(char *str);
void util_timestamp(char *buf, size_t len);

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <sys/stat.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef STELLI_USE_LIBCURL
#include <curl/curl.h>
#endif

/* -----------------------------------------------------------------------
 * Shared cancel / progress state (native path only)
 * ---------------------------------------------------------------------*/

static volatile bool *s_native_cancel_flag = NULL;
static volatile int  *s_native_upload_counter = NULL;

static int native_is_cancelled(void)
{
    return (s_native_cancel_flag && *s_native_cancel_flag) ? 1 : 0;
}

void ftp_native_set_cancel_flag(volatile bool *flag)
{
    s_native_cancel_flag = flag;
}

void ftp_native_set_upload_counter(volatile int *counter)
{
    s_native_upload_counter = counter;
}

/* -----------------------------------------------------------------------
 * Backend probe
 * ---------------------------------------------------------------------*/

int ftp_native_backend_available(void)
{
#ifdef STELLI_USE_LIBCURL
    return 1;
#else
    return 0;
#endif
}

const char *ftp_native_curl_version_string(void)
{
#ifdef STELLI_USE_LIBCURL
    return curl_version();
#else
    return "libcurl-disabled";
#endif
}

#ifdef STELLI_USE_LIBCURL

int ftp_native_protocols_ok(void)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    int has_ftp = 0;
    int has_sftp = 0;
    const char *const *p;

    if (!info || !info->protocols)
        return 0;

    for (p = info->protocols; *p; ++p) {
        if (strcmp(*p, "ftp") == 0 || strcmp(*p, "ftps") == 0)
            has_ftp = 1;
        if (strcmp(*p, "sftp") == 0 || strcmp(*p, "scp") == 0)
            has_sftp = 1;
    }

    (void)has_sftp; /* SFTP expected when built with libssh2; dispatch may log */
    return has_ftp ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Internals: protocol, paths, URL, curl session
 * ---------------------------------------------------------------------*/

static int native_use_sftp(int port)
{
    return (port == 22 || port == 2222 || port == 8827) ? 1 : 0;
}

/** Percent-encode path for URL; keep '/' unescaped. */
static void url_encode_path(const char *src, char *dest, size_t dest_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t n = 0;

    if (!src || !dest || dest_len == 0)
        return;

    while (*src && n + 4 < dest_len) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
            c == '/' || c == '@') {
            dest[n++] = (char)c;
        } else {
            dest[n++] = '%';
            dest[n++] = hex[c >> 4];
            dest[n++] = hex[c & 15];
        }
        src++;
    }
    dest[n] = '\0';
}

/**
 * Build scheme://host:port/path — credentials are NOT embedded (security).
 * remote_path should be absolute or start with '/'.
 */
static int build_remote_url(char *out, size_t out_len,
                            const char *host, int port, const char *remote_path)
{
    char enc_path[MAX_PATH_LEN * 3];
    const char *scheme = native_use_sftp(port) ? "sftp" : "ftp";
    const char *path = remote_path ? remote_path : "/";
    char path_buf[MAX_PATH_LEN];

    if (!out || out_len < 16 || !host || !host[0])
        return 0;

    /* Ensure path starts with '/' for absolute remote form. */
    if (path[0] != '/') {
        snprintf(path_buf, sizeof(path_buf), "/%s", path);
        path = path_buf;
    }

    url_encode_path(path, enc_path, sizeof(enc_path));

    /* Double-leading-slash after host keeps FTP absolute from root. */
    if (native_use_sftp(port)) {
        snprintf(out, out_len, "%s://%s:%d%s", scheme, host, port, enc_path);
    } else {
        snprintf(out, out_len, "%s://%s:%d/%s", scheme, host, port, enc_path);
    }
    return 1;
}

static void ensure_local_parent_dir(const char *local_path)
{
    char parent[MAX_PATH_LEN];
    char *last_sep;

    if (!local_path || !local_path[0])
        return;

    strncpy(parent, local_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';

    last_sep = strrchr(parent, '\\');
    if (!last_sep)
        last_sep = strrchr(parent, '/');
    if (last_sep && last_sep != parent) {
        *last_sep = '\0';
        util_ensure_directory(parent);
    }
}

static void resolve_abs_local(const char *local_path, char *abs_out, size_t abs_len)
{
#ifdef _WIN32
    if (!local_path || !abs_out || abs_len == 0)
        return;
    if (GetFullPathNameA(local_path, (DWORD)abs_len, abs_out, NULL) == 0) {
        strncpy(abs_out, local_path, abs_len - 1);
        abs_out[abs_len - 1] = '\0';
    }
#else
    if (!local_path || !abs_out || abs_len == 0)
        return;
    if (local_path[0] == '/') {
        strncpy(abs_out, local_path, abs_len - 1);
        abs_out[abs_len - 1] = '\0';
    } else {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd)))
            snprintf(abs_out, abs_len, "%s/%s", cwd, local_path);
        else {
            strncpy(abs_out, local_path, abs_len - 1);
            abs_out[abs_len - 1] = '\0';
        }
    }
#endif
}

static curl_off_t local_file_size(const char *path)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0)
        return -1;
    return (curl_off_t)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (curl_off_t)st.st_size;
#endif
}

/* -----------------------------------------------------------------------
 * SFTP host-key / known_hosts (transfer-security)
 * Config keys in config/ftp.ini — see config/ftp.ini.example.
 * ---------------------------------------------------------------------*/

#define NATIVE_DEFAULT_KNOWN_HOSTS "config/known_hosts"
#define NATIVE_FTP_INI             "config/ftp.ini"

typedef enum {
    HOST_KEY_POLICY_PIN = 0,   /* first-connect: accept + append to known_hosts */
    HOST_KEY_POLICY_FAIL = 1,  /* strict: reject unknown hosts and mismatches */
    HOST_KEY_POLICY_TRUST = 2  /* insecure: accept any key (debug/legacy only) */
} HostKeyPolicy;

typedef struct {
    char known_hosts[MAX_PATH_LEN];
    char host_key_pin[128]; /* SHA256 base64 (no "SHA256:" prefix for curl) */
    HostKeyPolicy policy;
    int loaded;
} NativeHostKeyConfig;

static NativeHostKeyConfig s_hostkey_cfg;

/** Ensure parent directory of path exists (e.g. "config" for config/known_hosts). */
static void native_ensure_parent_dir(const char *path)
{
    char parent[MAX_PATH_LEN];
    char *last_sep;

    if (!path || !path[0])
        return;
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    last_sep = strrchr(parent, '/');
#ifdef _WIN32
    {
        char *bs = strrchr(parent, '\\');
        if (bs && (!last_sep || bs > last_sep))
            last_sep = bs;
    }
#endif
    if (last_sep && last_sep != parent) {
        *last_sep = '\0';
        if (parent[0])
            util_ensure_directory(parent);
    }
}

/**
 * Touch an empty known_hosts file so CURLOPT_SSH_KNOWNHOSTS has a path
 * libcurl can open; first-connect pin appends via CURLKHSTAT_FINE_ADD_TO_FILE.
 */
static void native_ensure_known_hosts_file(const char *path)
{
    FILE *fp;
    if (!path || !path[0])
        return;
    if (util_file_exists(path))
        return;
    native_ensure_parent_dir(path);
    fp = fopen(path, "ab");
    if (fp)
        fclose(fp);
}

static void native_normalize_sha256_pin(const char *in, char *out, size_t out_len)
{
    const char *p = in;
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!p)
        return;
    while (*p == ' ' || *p == '\t')
        p++;
    /* Accept "SHA256:base64..." from ssh-keygen -lf style output. */
    if (strncmp(p, "SHA256:", 7) == 0 || strncmp(p, "sha256:", 7) == 0)
        p += 7;
    while (*p == ' ' || *p == '\t')
        p++;
    strncpy(out, p, out_len - 1);
    out[out_len - 1] = '\0';
    /* Trim trailing whitespace / CR */
    {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' ||
                         out[n - 1] == '\r' || out[n - 1] == '\n')) {
            out[--n] = '\0';
        }
    }
}

static HostKeyPolicy native_parse_host_key_policy(const char *s)
{
    if (!s || !s[0])
        return HOST_KEY_POLICY_PIN;
    if (util_strcasecmp(s, "fail") == 0 || util_strcasecmp(s, "strict") == 0 ||
        util_strcasecmp(s, "reject") == 0)
        return HOST_KEY_POLICY_FAIL;
    if (util_strcasecmp(s, "trust") == 0 || util_strcasecmp(s, "insecure") == 0 ||
        util_strcasecmp(s, "accept-all") == 0 || util_strcasecmp(s, "off") == 0)
        return HOST_KEY_POLICY_TRUST;
    /* pin / first-connect / default */
    return HOST_KEY_POLICY_PIN;
}

static const char *native_policy_name(HostKeyPolicy p)
{
    switch (p) {
    case HOST_KEY_POLICY_FAIL:  return "fail";
    case HOST_KEY_POLICY_TRUST: return "trust";
    case HOST_KEY_POLICY_PIN:
    default:                    return "pin";
    }
}

/**
 * Load host-key settings from config/ftp.ini (once per process, unless reloaded).
 * Defaults: known_hosts=config/known_hosts, policy=pin, no pin fingerprint.
 */
static void native_load_hostkey_config(void)
{
    char buf[MAX_PATH_LEN];

    if (s_hostkey_cfg.loaded)
        return;

    memset(&s_hostkey_cfg, 0, sizeof(s_hostkey_cfg));
    strncpy(s_hostkey_cfg.known_hosts, NATIVE_DEFAULT_KNOWN_HOSTS,
            sizeof(s_hostkey_cfg.known_hosts) - 1);
    s_hostkey_cfg.policy = HOST_KEY_POLICY_PIN;
    s_hostkey_cfg.loaded = 1;

    if (util_read_ini_value(NATIVE_FTP_INI, "KNOWN_HOSTS", buf, sizeof(buf)) &&
        buf[0]) {
        strncpy(s_hostkey_cfg.known_hosts, buf, sizeof(s_hostkey_cfg.known_hosts) - 1);
        s_hostkey_cfg.known_hosts[sizeof(s_hostkey_cfg.known_hosts) - 1] = '\0';
    }

    if (util_read_ini_value(NATIVE_FTP_INI, "HOST_KEY_PIN", buf, sizeof(buf)) &&
        buf[0]) {
        native_normalize_sha256_pin(buf, s_hostkey_cfg.host_key_pin,
                                    sizeof(s_hostkey_cfg.host_key_pin));
    }

    if (util_read_ini_value(NATIVE_FTP_INI, "HOST_KEY_POLICY", buf, sizeof(buf)) &&
        buf[0]) {
        s_hostkey_cfg.policy = native_parse_host_key_policy(buf);
    }

    util_log(SEVERITY_INFO,
             "Native SFTP host-key: known_hosts='%s' policy=%s pin=%s",
             s_hostkey_cfg.known_hosts,
             native_policy_name(s_hostkey_cfg.policy),
             s_hostkey_cfg.host_key_pin[0] ? "set" : "none");
}

/** Force re-read of KNOWN_HOSTS / HOST_KEY_PIN / HOST_KEY_POLICY from ftp.ini. */
void ftp_native_reload_security_config(void)
{
    s_hostkey_cfg.loaded = 0;
    native_load_hostkey_config();
}

/**
 * libcurl SSH host-key callback.
 * - MATCH_OK: accept
 * - MATCH_MISSING + pin: accept and append to known_hosts (first-connect pin)
 * - MATCH_MISSING + fail: reject
 * - MATCH_MISMATCH: always reject (unless policy=trust)
 * - policy=trust: accept all (INSECURE; logged)
 */
static int native_ssh_key_callback(CURL *easy,
                                   const struct curl_khkey *knownkey,
                                   const struct curl_khkey *foundkey,
                                   enum curl_khmatch match,
                                   void *clientp)
{
    NativeHostKeyConfig *cfg = (NativeHostKeyConfig *)clientp;
    (void)easy;
    (void)knownkey;
    (void)foundkey;

    if (!cfg)
        cfg = &s_hostkey_cfg;

    if (cfg->policy == HOST_KEY_POLICY_TRUST) {
        util_log(SEVERITY_WARNING,
                 "SFTP: HOST_KEY_POLICY=trust — accepting host key without "
                 "verification (INSECURE; legacy/debug only)");
        return CURLKHSTAT_FINE;
    }

    switch (match) {
    case CURLKHMATCH_OK:
        return CURLKHSTAT_FINE;

    case CURLKHMATCH_MISSING:
        if (cfg->policy == HOST_KEY_POLICY_PIN) {
            native_ensure_known_hosts_file(cfg->known_hosts);
            util_log(SEVERITY_INFO,
                     "SFTP: first-connect pin — accepting host key and writing "
                     "to '%s'",
                     cfg->known_hosts[0] ? cfg->known_hosts
                                         : NATIVE_DEFAULT_KNOWN_HOSTS);
            return CURLKHSTAT_FINE_ADD_TO_FILE;
        }
        util_log(SEVERITY_ERROR,
                 "SFTP: host key not in known_hosts (HOST_KEY_POLICY=fail). "
                 "Add the server key to '%s' or set HOST_KEY_PIN.",
                 cfg->known_hosts[0] ? cfg->known_hosts
                                     : NATIVE_DEFAULT_KNOWN_HOSTS);
        return CURLKHSTAT_REJECT;

    case CURLKHMATCH_MISMATCH:
        util_log(SEVERITY_ERROR,
                 "SFTP: HOST KEY MISMATCH for known host — refusing connection "
                 "(possible MITM). Update known_hosts only if the server key "
                 "was intentionally rotated.");
        return CURLKHSTAT_REJECT;

    default:
        util_log(SEVERITY_ERROR, "SFTP: unknown host-key match result (%d)",
                 (int)match);
        return CURLKHSTAT_REJECT;
    }
}

static int native_xferinfo(void *clientp,
                           curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    if (native_is_cancelled())
        return 1; /* abort transfer */
    return 0;
}

static CURL *native_curl_init_session(const char *host, int port,
                                      const char *user, const char *pass)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;

    (void)host; /* host is in URL; kept for future session reuse */

    /*
     * Credential hygiene: USER/PASS only via curl options (in-process).
     * Never embed credentials in the URL, argv, CreateProcess command line,
     * or plaintext .TEMP scripts on the native path.
     */
    curl_easy_setopt(curl, CURLOPT_USERNAME, user ? user : "");
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass ? pass : "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); /* no overall timeout; cancel handles abort */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, native_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);

    if (native_use_sftp(port)) {
        native_load_hostkey_config();

        curl_easy_setopt(curl, CURLOPT_SSH_AUTH_TYPES,
                         (long)(CURLSSH_AUTH_PASSWORD | CURLSSH_AUTH_KEYBOARD));

        /* known_hosts file (OpenSSH format); first-connect may append. */
        if (s_hostkey_cfg.known_hosts[0]) {
            if (s_hostkey_cfg.policy == HOST_KEY_POLICY_PIN)
                native_ensure_known_hosts_file(s_hostkey_cfg.known_hosts);
            else
                native_ensure_parent_dir(s_hostkey_cfg.known_hosts);
            curl_easy_setopt(curl, CURLOPT_SSH_KNOWNHOSTS,
                             s_hostkey_cfg.known_hosts);
        }

        /* Optional explicit SHA256 fingerprint pin (base64). */
        if (s_hostkey_cfg.host_key_pin[0]) {
            curl_easy_setopt(curl, CURLOPT_SSH_HOST_PUBLIC_KEY_SHA256,
                             s_hostkey_cfg.host_key_pin);
        }

        curl_easy_setopt(curl, CURLOPT_SSH_KEYFUNCTION, native_ssh_key_callback);
        curl_easy_setopt(curl, CURLOPT_SSH_KEYDATA, &s_hostkey_cfg);
    } else {
        /* Passive FTP is default; create missing dirs on upload path. */
        curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
                         (long)CURLFTP_CREATE_DIR_RETRY);
    }

    /* Also enable create-missing for SFTP (supported by modern libcurl). */
    curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
                     (long)CURLFTP_CREATE_DIR_RETRY);

    return curl;
}

static int g_curl_global_inited = 0;

static void native_ensure_curl_global(void)
{
    if (!g_curl_global_inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curl_global_inited = 1;
    }
}

/* -----------------------------------------------------------------------
 * WRITE / READ helpers
 * ---------------------------------------------------------------------*/

static size_t write_file_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *fp = (FILE *)userdata;
    if (native_is_cancelled())
        return 0; /* short write => abort */
    return fwrite(ptr, size, nmemb, fp);
}

static size_t read_file_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *fp = (FILE *)userdata;
    if (native_is_cancelled())
        return CURL_READFUNC_ABORT;
    return fread(ptr, size, nmemb, fp);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MemBuf;

static size_t write_membuf_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    MemBuf *mb = (MemBuf *)userdata;
    size_t n = size * nmemb;
    if (native_is_cancelled())
        return 0;
    if (mb->len + n + 1 > mb->cap) {
        size_t ncap = mb->cap ? mb->cap * 2 : 4096;
        while (ncap < mb->len + n + 1)
            ncap *= 2;
        char *nd = (char *)realloc(mb->data, ncap);
        if (!nd)
            return 0;
        mb->data = nd;
        mb->cap = ncap;
    }
    memcpy(mb->data + mb->len, ptr, n);
    mb->len += n;
    mb->data[mb->len] = '\0';
    return n;
}

/* -----------------------------------------------------------------------
 * Single-file download / upload
 * ---------------------------------------------------------------------*/

bool ftp_native_download_file(const char *host, int port, const char *user, const char *pass,
                              const char *remote_path, const char *local_path)
{
    CURL *curl;
    CURLcode rc;
    char url[MAX_PATH_LEN * 3];
    char abs_local[MAX_PATH_LEN];
    FILE *fp;
    long http_code = 0;

    if (!host || !remote_path || !local_path) {
        util_log(SEVERITY_ERROR, "ftp_native_download_file: missing arguments");
        return false;
    }
    if (native_is_cancelled())
        return false;

    native_ensure_curl_global();
    resolve_abs_local(local_path, abs_local, sizeof(abs_local));
    ensure_local_parent_dir(abs_local);

    if (!build_remote_url(url, sizeof(url), host, port, remote_path)) {
        util_log(SEVERITY_ERROR, "ftp_native_download_file: bad URL");
        return false;
    }

    fp = fopen(abs_local, "wb");
    if (!fp) {
        util_log(SEVERITY_ERROR, "ftp_native_download_file: cannot open '%s' (%s)",
                 abs_local, strerror(errno));
        return false;
    }

    curl = native_curl_init_session(host, port, user, pass);
    if (!curl) {
        fclose(fp);
        util_log(SEVERITY_ERROR, "ftp_native_download_file: curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    util_log(SEVERITY_INFO, "Native %s GET %s -> %s",
             native_use_sftp(port) ? "SFTP" : "FTP", remote_path, abs_local);

    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (native_is_cancelled()) {
        remove(abs_local);
        util_log(SEVERITY_WARNING, "ftp_native_download_file: cancelled");
        return false;
    }

    if (rc != CURLE_OK) {
        util_log(SEVERITY_ERROR, "ftp_native_download_file: %s (%d) url=%s",
                 curl_easy_strerror(rc), (int)rc, url);
        remove(abs_local);
        return false;
    }

    return true;
}

bool ftp_native_upload_file(const char *host, int port, const char *user, const char *pass,
                            const char *local_path, const char *remote_path)
{
    CURL *curl;
    CURLcode rc;
    char url[MAX_PATH_LEN * 3];
    char abs_local[MAX_PATH_LEN];
    FILE *fp;
    curl_off_t fsize;

    if (!host || !local_path || !remote_path) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_file: missing arguments");
        return false;
    }
    if (native_is_cancelled())
        return false;

    resolve_abs_local(local_path, abs_local, sizeof(abs_local));
    if (!util_file_exists(abs_local)) {
        util_log(SEVERITY_WARNING, "Upload skipped: Local file not found '%s'", abs_local);
        return false;
    }

    fsize = local_file_size(abs_local);
    if (fsize < 0) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_file: cannot stat '%s'", abs_local);
        return false;
    }

    native_ensure_curl_global();
    if (!build_remote_url(url, sizeof(url), host, port, remote_path)) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_file: bad URL");
        return false;
    }

    fp = fopen(abs_local, "rb");
    if (!fp) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_file: cannot open '%s'", abs_local);
        return false;
    }

    curl = native_curl_init_session(host, port, user, pass);
    if (!curl) {
        fclose(fp);
        util_log(SEVERITY_ERROR, "ftp_native_upload_file: curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_file_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, fsize);

    util_log(SEVERITY_INFO, "Native %s PUT %s -> %s",
             native_use_sftp(port) ? "SFTP" : "FTP", abs_local, remote_path);

    rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (native_is_cancelled()) {
        util_log(SEVERITY_WARNING, "ftp_native_upload_file: cancelled");
        return false;
    }

    if (rc != CURLE_OK) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_file: %s (%d)",
                 curl_easy_strerror(rc), (int)rc);
        return false;
    }

    if (s_native_upload_counter)
        (*s_native_upload_counter)++;

    return true;
}

/* -----------------------------------------------------------------------
 * Batch upload / download
 * ---------------------------------------------------------------------*/

bool ftp_native_upload_batch(const char *host, int port, const char *user, const char *pass,
                             const char **local_paths, const char **remote_paths, int count,
                             int *out_succeeded, int *out_failed)
{
    int succeeded = 0;
    int failed = 0;
    int i;

    if (!local_paths || !remote_paths || count <= 0) {
        if (out_succeeded) *out_succeeded = 0;
        if (out_failed) *out_failed = 0;
        return false;
    }

    util_log(SEVERITY_INFO, "ftp_native_upload_batch: %d file(s) via libcurl (%s)",
             count, native_use_sftp(port) ? "SFTP" : "FTP");

    for (i = 0; i < count; i++) {
        if (native_is_cancelled()) {
            util_log(SEVERITY_WARNING, "ftp_native_upload_batch: cancelled at %d/%d",
                     i, count);
            failed += (count - i);
            break;
        }
        if (!local_paths[i] || !remote_paths[i]) {
            failed++;
            continue;
        }
        if (ftp_native_upload_file(host, port, user, pass,
                                   local_paths[i], remote_paths[i])) {
            succeeded++;
        } else {
            failed++;
        }
    }

    if (out_succeeded) *out_succeeded = succeeded;
    if (out_failed) *out_failed = failed;

    if (succeeded > 0 && failed == 0) {
        util_log(SEVERITY_INFO, "ftp_native_upload_batch: All %d file(s) uploaded.",
                 succeeded);
        return true;
    }
    if (succeeded > 0) {
        util_log(SEVERITY_WARNING,
                 "ftp_native_upload_batch: partial — %d ok, %d failed",
                 succeeded, failed);
        return false;
    }
    util_log(SEVERITY_ERROR, "ftp_native_upload_batch: all uploads failed (%d)", failed);
    return false;
}

bool ftp_native_download_batch(const char *host, int port, const char *user, const char *pass,
                               const char **remote_paths, const char **local_paths, int count)
{
    int ok = 0;
    int i;

    if (!remote_paths || !local_paths || count <= 0)
        return false;

    util_log(SEVERITY_INFO, "ftp_native_download_batch: %d file(s) via libcurl (%s)",
             count, native_use_sftp(port) ? "SFTP" : "FTP");

    for (i = 0; i < count; i++) {
        if (native_is_cancelled()) {
            util_log(SEVERITY_WARNING, "ftp_native_download_batch: cancelled at %d/%d",
                     i, count);
            return false;
        }
        if (!remote_paths[i] || !local_paths[i])
            continue;
        if (ftp_native_download_file(host, port, user, pass,
                                     remote_paths[i], local_paths[i])) {
            ok++;
        }
    }

    return ok > 0;
}

/* -----------------------------------------------------------------------
 * Directory listing
 * ---------------------------------------------------------------------*/

static void join_remote_path(const char *dir, const char *name,
                             char *out, size_t out_len)
{
    size_t dlen = dir ? strlen(dir) : 0;
    if (!dir || dlen == 0) {
        snprintf(out, out_len, "/%s", name ? name : "");
        return;
    }
    if (dir[dlen - 1] == '/')
        snprintf(out, out_len, "%s%s", dir, name ? name : "");
    else
        snprintf(out, out_len, "%s/%s", dir, name ? name : "");
}

static int cmp_remote_entries_native(const void *a, const void *b)
{
    const RemoteFileEntry *ea = (const RemoteFileEntry *)a;
    const RemoteFileEntry *eb = (const RemoteFileEntry *)b;
    if (ea->is_directory != eb->is_directory)
        return (int)eb->is_directory - (int)ea->is_directory;
    return strcmp(ea->name, eb->name);
}

/**
 * Parse one LIST line (Unix or DOS/Windows FTP style) into entry.
 * Returns 1 on success.
 */
static int parse_list_line(const char *line, RemoteFileEntry *entry)
{
    const char *p;
    char name_buf[256];

    if (!line || !entry)
        return 0;

    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0' || *line == '\r' || *line == '\n')
        return 0;

    /* Skip "total N" summary lines */
    if (strncmp(line, "total ", 6) == 0)
        return 0;

    memset(entry, 0, sizeof(*entry));

    /* ---- Unix-style: drwxr-xr-x ... name ---- */
    if ((line[0] == 'd' || line[0] == '-' || line[0] == 'l') &&
        strlen(line) > 10 && line[1] != '\0' &&
        (line[1] == 'r' || line[1] == '-')) {
        int field = 0;
        const char *name_start = NULL;
        long long size_val = 0;

        entry->is_directory = (line[0] == 'd');

        p = line;
        /* skip mode */
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;

        /* fields: links owner group size month day time/year name */
        while (*p && field < 8) {
            if (field == 3) {
                /* size */
                size_val = 0;
                while (*p >= '0' && *p <= '9') {
                    size_val = size_val * 10 + (*p - '0');
                    p++;
                }
                entry->size = size_val;
                while (*p == ' ') p++;
                field++;
                continue;
            }
            if (field >= 4 && field <= 6) {
                /* date parts — skip tokens */
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
                field++;
                continue;
            }
            /* links / owner / group */
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            field++;
        }
        name_start = p;
        if (!name_start || !*name_start)
            return 0;

        strncpy(name_buf, name_start, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        {
            int len = (int)strlen(name_buf);
            while (len > 0 && (name_buf[len - 1] == '\n' || name_buf[len - 1] == '\r' ||
                               name_buf[len - 1] == ' '))
                name_buf[--len] = '\0';
        }
        /* symlink: "name -> target" — keep left side */
        {
            char *arrow = strstr(name_buf, " -> ");
            if (arrow)
                *arrow = '\0';
        }
        if (name_buf[0] == '\0' || strcmp(name_buf, ".") == 0 || strcmp(name_buf, "..") == 0)
            return 0;
        strncpy(entry->name, name_buf, sizeof(entry->name) - 1);
        return 1;
    }

    /* ---- DOS / Windows FTP style:
     * 01-15-25  04:21PM       <DIR>          Addons
     * 01-15-25  04:21PM                 6796 types.xml
     */
    if (isdigit((unsigned char)line[0]) && strlen(line) > 17) {
        const char *rest;
        char date_part[32];

        /* date + time ~ first two tokens */
        p = line;
        {
            int di = 0;
            while (*p && *p != ' ' && di < (int)sizeof(date_part) - 1)
                date_part[di++] = *p++;
            date_part[di++] = ' ';
            while (*p == ' ') p++;
            while (*p && *p != ' ' && di < (int)sizeof(date_part) - 1)
                date_part[di++] = *p++;
            date_part[di] = '\0';
            strncpy(entry->date_str, date_part, sizeof(entry->date_str) - 1);
        }
        while (*p == ' ') p++;
        rest = p;

        if (strncmp(rest, "<DIR>", 5) == 0 || strncmp(rest, "<dir>", 5) == 0) {
            entry->is_directory = true;
            entry->size = 0;
            rest += 5;
            while (*rest == ' ') rest++;
        } else {
            entry->is_directory = false;
            entry->size = 0;
            while (*rest >= '0' && *rest <= '9') {
                entry->size = entry->size * 10 + (*rest - '0');
                rest++;
            }
            while (*rest == ' ') rest++;
        }
        if (!*rest)
            return 0;
        strncpy(name_buf, rest, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        {
            int len = (int)strlen(name_buf);
            while (len > 0 && (name_buf[len - 1] == '\n' || name_buf[len - 1] == '\r' ||
                               name_buf[len - 1] == ' '))
                name_buf[--len] = '\0';
        }
        if (name_buf[0] == '\0' || strcmp(name_buf, ".") == 0 || strcmp(name_buf, "..") == 0)
            return 0;
        strncpy(entry->name, name_buf, sizeof(entry->name) - 1);
        return 1;
    }

    return 0;
}

bool ftp_native_list_directory(const char *host, int port, const char *user, const char *pass,
                               const char *remote_path, RemoteFileBrowser *browser)
{
    CURL *curl;
    CURLcode rc;
    char url[MAX_PATH_LEN * 3];
    char list_path[MAX_PATH_LEN];
    MemBuf mb = {0};
    char *line;
    char *save;
    size_t rlen;

    if (!browser)
        return false;

    browser->count = 0;
    browser->error[0] = '\0';
    if (remote_path)
        strncpy(browser->current_path, remote_path, sizeof(browser->current_path) - 1);
    else
        browser->current_path[0] = '\0';
    browser->current_path[sizeof(browser->current_path) - 1] = '\0';

    if (!host || !remote_path) {
        snprintf(browser->error, sizeof(browser->error), "missing host/path");
        return false;
    }
    if (native_is_cancelled()) {
        snprintf(browser->error, sizeof(browser->error), "cancelled");
        return false;
    }

    /* Directory URL should end with '/' so LIST is on the dir, not a file. */
    rlen = strlen(remote_path);
    if (rlen > 0 && remote_path[rlen - 1] == '/') {
        strncpy(list_path, remote_path, sizeof(list_path) - 1);
        list_path[sizeof(list_path) - 1] = '\0';
    } else {
        snprintf(list_path, sizeof(list_path), "%s/", remote_path);
    }

    native_ensure_curl_global();
    if (!build_remote_url(url, sizeof(url), host, port, list_path)) {
        snprintf(browser->error, sizeof(browser->error), "bad URL");
        return false;
    }

    curl = native_curl_init_session(host, port, user, pass);
    if (!curl) {
        snprintf(browser->error, sizeof(browser->error), "curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L); /* full LIST for type/size */

    util_log(SEVERITY_INFO, "Native %s LIST %s",
             native_use_sftp(port) ? "SFTP" : "FTP", remote_path);

    rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (native_is_cancelled()) {
        free(mb.data);
        snprintf(browser->error, sizeof(browser->error), "cancelled");
        return false;
    }

    if (rc != CURLE_OK) {
        snprintf(browser->error, sizeof(browser->error), "%s", curl_easy_strerror(rc));
        util_log(SEVERITY_ERROR, "ftp_native_list_directory: %s", browser->error);
        free(mb.data);
        return false;
    }

    if (!mb.data || mb.len == 0) {
        util_log(SEVERITY_INFO, "Browser: empty listing for '%s'", remote_path);
        free(mb.data);
        return true;
    }

    /* Portable line walk (avoid strtok_r — not on MSVC). */
    line = mb.data;
    while (line && *line && browser->count < MAX_BROWSER_ENTRIES) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
            save = nl + 1;
        } else {
            save = NULL;
        }
        /* trim CR */
        {
            size_t L = strlen(line);
            if (L > 0 && line[L - 1] == '\r')
                line[L - 1] = '\0';
        }
        {
            RemoteFileEntry entry;
            if (parse_list_line(line, &entry)) {
                join_remote_path(remote_path, entry.name,
                                 entry.full_path, sizeof(entry.full_path));
                browser->entries[browser->count++] = entry;
            }
        }
        line = save;
    }

    free(mb.data);

    qsort(browser->entries, (size_t)browser->count, sizeof(RemoteFileEntry),
          cmp_remote_entries_native);

    util_log(SEVERITY_INFO, "Browser: Listed %d entries in '%s' (native)",
             browser->count, remote_path);
    return true;
}

/* -----------------------------------------------------------------------
 * Advanced ops: recursive download, core files, upload dir, cleanup,
 * verify, restore (no WinSCP / no log scraping)
 * ---------------------------------------------------------------------*/

/** Exclude directory basenames (match WinSCP foundation filemasks). */
static int native_is_excluded_dirname(const char *name)
{
    static const char *const dirs[] = {
        "_CommonRedist", "steamapps", "battleye", "docs", "keys", "addons",
        "appcache", "logs", "dta", "HostHavocDayZServer", "server_manager",
        "backups", "config", "storage_1", "profiles",
        "Key", "key", "info", "extra", "extras", "Extras", "Readme_Terms",
        NULL
    };
    int i;
    if (!name || !name[0])
        return 1;
    for (i = 0; dirs[i]; i++) {
        if (util_strcasecmp(name, dirs[i]) == 0)
            return 1;
    }
    return 0;
}

/** Exclude binary/log extensions (match WinSCP foundation filemasks). */
static int native_is_excluded_extension(const char *name)
{
    static const char *const exts[] = {
        ".pbo", ".bin", ".exe", ".dll", ".vdf", ".mdmp", ".rpt", ".log", ".adm",
        NULL
    };
    const char *dot;
    int i;
    if (!name)
        return 1;
    dot = strrchr(name, '.');
    if (!dot || !dot[1])
        return 0;
    for (i = 0; exts[i]; i++) {
        if (util_strcasecmp(dot, exts[i]) == 0)
            return 1;
    }
    return 0;
}

/** include_mask bits: which extensions to accept. */
enum {
    NINC_XML  = 1 << 0,
    NINC_JSON = 1 << 1,
    NINC_CFG  = 1 << 2,
    NINC_INI  = 1 << 3,
    NINC_TXT  = 1 << 4,
    NINC_MD   = 1 << 5
};

static int native_ext_allowed(const char *name, unsigned include_mask)
{
    const char *dot;
    if (!name)
        return 0;
    if (native_is_excluded_extension(name))
        return 0;
    dot = strrchr(name, '.');
    if (!dot)
        return 0;
    if ((include_mask & NINC_XML)  && util_strcasecmp(dot, ".xml")  == 0) return 1;
    if ((include_mask & NINC_JSON) && util_strcasecmp(dot, ".json") == 0) return 1;
    if ((include_mask & NINC_CFG)  && util_strcasecmp(dot, ".cfg")  == 0) return 1;
    if ((include_mask & NINC_INI)  && util_strcasecmp(dot, ".ini")  == 0) return 1;
    if ((include_mask & NINC_TXT)  && util_strcasecmp(dot, ".txt")  == 0) return 1;
    if ((include_mask & NINC_MD)   && util_strcasecmp(dot, ".md")   == 0) return 1;
    return 0;
}

static void native_join_local(const char *dir, const char *name, char *out, size_t out_len)
{
    size_t dlen = dir ? strlen(dir) : 0;
    if (!dir || dlen == 0) {
        snprintf(out, out_len, "%s", name ? name : "");
        return;
    }
#ifdef _WIN32
    if (dir[dlen - 1] == '\\' || dir[dlen - 1] == '/')
        snprintf(out, out_len, "%s%s", dir, name ? name : "");
    else
        snprintf(out, out_len, "%s\\%s", dir, name ? name : "");
#else
    if (dir[dlen - 1] == '/')
        snprintf(out, out_len, "%s%s", dir, name ? name : "");
    else
        snprintf(out, out_len, "%s/%s", dir, name ? name : "");
#endif
}

/**
 * Probe remote file size via CURLOPT_NOBODY (FTP SIZE / SFTP stat).
 * Returns size >= 0 on success, -1 on failure / missing.
 */
static curl_off_t native_remote_file_size(const char *host, int port,
                                          const char *user, const char *pass,
                                          const char *remote_path)
{
    CURL *curl;
    CURLcode rc;
    char url[MAX_PATH_LEN * 3];
    curl_off_t clen = -1;
    double clen_d = -1.0;

    if (!host || !remote_path || !remote_path[0])
        return -1;
    if (native_is_cancelled())
        return -1;

    native_ensure_curl_global();
    if (!build_remote_url(url, sizeof(url), host, port, remote_path))
        return -1;

    curl = native_curl_init_session(host, port, user, pass);
    if (!curl)
        return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf_cb);
    /* Discard any body into a tiny throwaway membuf. */
    {
        MemBuf mb = {0};
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
        rc = curl_easy_perform(curl);
        free(mb.data);
    }

    if (rc == CURLE_OK) {
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &clen) != CURLE_OK ||
            clen < 0) {
            if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &clen_d) == CURLE_OK &&
                clen_d >= 0.0)
                clen = (curl_off_t)clen_d;
            else
                clen = -1;
        }
    } else {
        clen = -1;
    }

    curl_easy_cleanup(curl);
    return clen;
}

/** Delete one remote file (FTP DELE / SFTP rm). Best-effort. */
static int native_delete_remote_file(const char *host, int port,
                                     const char *user, const char *pass,
                                     const char *remote_path)
{
    CURL *curl;
    CURLcode rc;
    char url[MAX_PATH_LEN * 3];
    int ok = 0;

    if (!host || !remote_path || !remote_path[0])
        return 0;
    if (native_is_cancelled())
        return 0;

    native_ensure_curl_global();
    curl = native_curl_init_session(host, port, user, pass);
    if (!curl)
        return 0;

    if (native_use_sftp(port)) {
        /* SFTP QUOTE rm requires a connection URL (use parent dir if possible). */
        char parent[MAX_PATH_LEN];
        char quote_cmd[MAX_PATH_LEN + 16];
        char *slash;
        struct curl_slist *cmds = NULL;

        strncpy(parent, remote_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        slash = strrchr(parent, '/');
        if (slash && slash != parent)
            *slash = '\0';
        else
            strncpy(parent, "/", sizeof(parent) - 1);

        if (parent[0] && parent[strlen(parent) - 1] != '/') {
            size_t pl = strlen(parent);
            if (pl + 1 < sizeof(parent)) {
                parent[pl] = '/';
                parent[pl + 1] = '\0';
            }
        }

        if (!build_remote_url(url, sizeof(url), host, port, parent)) {
            curl_easy_cleanup(curl);
            return 0;
        }
        /* libcurl SFTP quote: rm /absolute/path */
        snprintf(quote_cmd, sizeof(quote_cmd), "rm %s", remote_path);
        cmds = curl_slist_append(cmds, quote_cmd);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_QUOTE, cmds);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        rc = curl_easy_perform(curl);
        curl_slist_free_all(cmds);
        ok = (rc == CURLE_OK) ? 1 : 0;
        if (!ok)
            util_log(SEVERITY_WARNING, "native_delete_remote_file(SFTP): %s — %s",
                     remote_path, curl_easy_strerror(rc));
    } else {
        if (!build_remote_url(url, sizeof(url), host, port, remote_path)) {
            curl_easy_cleanup(curl);
            return 0;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELE");
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf_cb);
        {
            MemBuf mb = {0};
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
            rc = curl_easy_perform(curl);
            free(mb.data);
        }
        ok = (rc == CURLE_OK) ? 1 : 0;
        if (!ok)
            util_log(SEVERITY_WARNING, "native_delete_remote_file(FTP): %s — %s",
                     remote_path, curl_easy_strerror(rc));
    }

    curl_easy_cleanup(curl);
    return ok;
}

/**
 * Walk remote tree and download files matching include_mask.
 * Size-based skip: if local exists and sizes match, skip transfer.
 */
static bool native_download_tree(const char *host, int port,
                                 const char *user, const char *pass,
                                 const char *remote_dir, const char *local_dir,
                                 unsigned include_mask, const char *label)
{
#define NATIVE_DIR_STACK_MAX 512
    char stack[NATIVE_DIR_STACK_MAX][MAX_PATH_LEN];
    char local_stack[NATIVE_DIR_STACK_MAX][MAX_PATH_LEN];
    int sp = 0;
    int downloaded = 0;
    int skipped = 0;
    int failed = 0;
    int listed_dirs = 0;
    char abs_local_root[MAX_PATH_LEN];

    if (!host || !remote_dir || !local_dir) {
        util_log(SEVERITY_ERROR, "%s: missing arguments", label);
        return false;
    }

    resolve_abs_local(local_dir, abs_local_root, sizeof(abs_local_root));
    util_ensure_directory(abs_local_root);

    util_log(SEVERITY_INFO,
             "Hunter(native): Incremental download from '%s' — size-match skip; %s",
             remote_dir, label);

    strncpy(stack[0], remote_dir, MAX_PATH_LEN - 1);
    stack[0][MAX_PATH_LEN - 1] = '\0';
    strncpy(local_stack[0], abs_local_root, MAX_PATH_LEN - 1);
    local_stack[0][MAX_PATH_LEN - 1] = '\0';
    sp = 1;

    while (sp > 0) {
        char cur_remote[MAX_PATH_LEN];
        char cur_local[MAX_PATH_LEN];
        RemoteFileBrowser browser;
        int i;

        if (native_is_cancelled()) {
            util_log(SEVERITY_WARNING, "%s: cancelled", label);
            return downloaded > 0;
        }

        sp--;
        strncpy(cur_remote, stack[sp], sizeof(cur_remote) - 1);
        cur_remote[sizeof(cur_remote) - 1] = '\0';
        strncpy(cur_local, local_stack[sp], sizeof(cur_local) - 1);
        cur_local[sizeof(cur_local) - 1] = '\0';

        memset(&browser, 0, sizeof(browser));
        if (!ftp_native_list_directory(host, port, user, pass, cur_remote, &browser)) {
            util_log(SEVERITY_WARNING, "%s: list failed for '%s': %s",
                     label, cur_remote, browser.error[0] ? browser.error : "?");
            failed++;
            continue;
        }
        listed_dirs++;

        for (i = 0; i < browser.count; i++) {
            RemoteFileEntry *e = &browser.entries[i];
            char child_local[MAX_PATH_LEN];

            if (native_is_cancelled())
                break;

            if (e->is_directory) {
                if (native_is_excluded_dirname(e->name))
                    continue;
                if (sp >= NATIVE_DIR_STACK_MAX) {
                    util_log(SEVERITY_WARNING, "%s: dir stack full, skip '%s'",
                             label, e->full_path);
                    continue;
                }
                native_join_local(cur_local, e->name, child_local, sizeof(child_local));
                util_ensure_directory(child_local);
                strncpy(stack[sp], e->full_path, MAX_PATH_LEN - 1);
                stack[sp][MAX_PATH_LEN - 1] = '\0';
                strncpy(local_stack[sp], child_local, MAX_PATH_LEN - 1);
                local_stack[sp][MAX_PATH_LEN - 1] = '\0';
                sp++;
                continue;
            }

            if (!native_ext_allowed(e->name, include_mask))
                continue;

            native_join_local(cur_local, e->name, child_local, sizeof(child_local));

            /* Size-based sync (WinSCP -criteria=size parity intent). */
            if (util_file_exists(child_local)) {
                curl_off_t lsz = local_file_size(child_local);
                if (lsz >= 0 && e->size >= 0 && lsz == (curl_off_t)e->size) {
                    skipped++;
                    continue;
                }
            }

            if (ftp_native_download_file(host, port, user, pass,
                                         e->full_path, child_local)) {
                downloaded++;
            } else {
                failed++;
            }
        }
    }

    util_log(SEVERITY_INFO,
             "%s: done — %d downloaded, %d skipped (size match), %d failed, %d dirs listed",
             label, downloaded, skipped, failed, listed_dirs);

    /* Success if we walked something and were not fully empty of progress
     * OR remote was empty (listed at least root). Match consumer expectations:
     * true when no hard total failure. */
    if (native_is_cancelled() && downloaded == 0)
        return false;
    if (listed_dirs == 0 && downloaded == 0)
        return false;
    return true;
#undef NATIVE_DIR_STACK_MAX
}

bool ftp_native_download_recursive(const char *host, int port, const char *user, const char *pass,
                                   const char *remote_dir, const char *local_dir)
{
    unsigned mask = NINC_XML;
    char flag[8] = {0};

    if (util_read_ini_value("config/server_paths.ini", "DOWNLOAD_CONFIG_FILES",
                            flag, sizeof(flag)) &&
        atoi(flag) == 1) {
        mask = NINC_XML | NINC_JSON | NINC_CFG | NINC_INI | NINC_TXT;
    }

    return native_download_tree(host, port, user, pass, remote_dir, local_dir,
                                mask, "ftp_native_download_recursive");
}

bool ftp_native_download_core_files(const char *host, int port, const char *user, const char *pass,
                                    const char *remote_root, const char *local_root)
{
    return native_download_tree(host, port, user, pass, remote_root, local_root,
                                NINC_XML | NINC_JSON, "ftp_native_download_core_files");
}

/**
 * Recursive local walk → upload matching files (xml/json/txt/md).
 * Mirrors WinSCP synchronize remote -filemask="*.xml;*.json;*.txt;*.md".
 */
static int native_upload_tree_walk(const char *host, int port,
                                   const char *user, const char *pass,
                                   const char *local_dir, const char *remote_dir,
                                   int *ok_count, int *fail_count)
{
#ifdef _WIN32
    char search[MAX_PATH_LEN];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;

    snprintf(search, sizeof(search), "%s\\*", local_dir);
    hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;

    do {
        char local_child[MAX_PATH_LEN];
        char remote_child[MAX_PATH_LEN];

        if (native_is_cancelled())
            break;
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        native_join_local(local_dir, fd.cFileName, local_child, sizeof(local_child));
        join_remote_path(remote_dir, fd.cFileName, remote_child, sizeof(remote_child));

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (native_is_excluded_dirname(fd.cFileName))
                continue;
            native_upload_tree_walk(host, port, user, pass,
                                    local_child, remote_child, ok_count, fail_count);
        } else {
            if (!native_ext_allowed(fd.cFileName,
                                    NINC_XML | NINC_JSON | NINC_TXT | NINC_MD))
                continue;
            if (ftp_native_upload_file(host, port, user, pass,
                                       local_child, remote_child)) {
                (*ok_count)++;
            } else {
                (*fail_count)++;
            }
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return 1;
#else
    DIR *d = opendir(local_dir);
    struct dirent *ent;
    if (!d)
        return 0;
    while ((ent = readdir(d)) != NULL) {
        char local_child[MAX_PATH_LEN];
        char remote_child[MAX_PATH_LEN];
        struct stat st;

        if (native_is_cancelled())
            break;
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        native_join_local(local_dir, ent->d_name, local_child, sizeof(local_child));
        join_remote_path(remote_dir, ent->d_name, remote_child, sizeof(remote_child));

        if (stat(local_child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (native_is_excluded_dirname(ent->d_name))
                continue;
            native_upload_tree_walk(host, port, user, pass,
                                    local_child, remote_child, ok_count, fail_count);
        } else if (S_ISREG(st.st_mode)) {
            if (!native_ext_allowed(ent->d_name,
                                    NINC_XML | NINC_JSON | NINC_TXT | NINC_MD))
                continue;
            if (ftp_native_upload_file(host, port, user, pass,
                                       local_child, remote_child)) {
                (*ok_count)++;
            } else {
                (*fail_count)++;
            }
        }
    }
    closedir(d);
    return 1;
#endif
}

bool ftp_native_upload_directory(const char *host, int port, const char *user, const char *pass,
                                 const char *local_dir, const char *remote_dir)
{
    char abs_local[MAX_PATH_LEN];
    int ok_count = 0;
    int fail_count = 0;

    if (!host || !local_dir || !remote_dir) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_directory: missing arguments");
        return false;
    }

    resolve_abs_local(local_dir, abs_local, sizeof(abs_local));
    util_log(SEVERITY_INFO,
             "ftp_native_upload_directory: %s -> %s (xml/json/txt/md)",
             abs_local, remote_dir);

    if (!native_upload_tree_walk(host, port, user, pass, abs_local, remote_dir,
                                 &ok_count, &fail_count)) {
        util_log(SEVERITY_ERROR, "ftp_native_upload_directory: cannot open '%s'",
                 abs_local);
        return false;
    }

    util_log(SEVERITY_INFO, "ftp_native_upload_directory: %d ok, %d failed",
             ok_count, fail_count);
    if (native_is_cancelled() && ok_count == 0)
        return false;
    return ok_count > 0 && fail_count == 0 ? true : (ok_count > 0);
}

bool ftp_native_cleanup_remote_dir(const char *host, int port, const char *user, const char *pass,
                                   const char *remote_dir)
{
    RemoteFileBrowser browser;
    int i;
    int removed = 0;

    if (!host || !user || !pass || !remote_dir)
        return false;

    /* Pattern: NNN__* (3+ digit prefix + double underscore) — numbered artifacts. */
    util_log(SEVERITY_INFO,
             "Cleanup(native): Removing numbered artifact files from %s/", remote_dir);

    memset(&browser, 0, sizeof(browser));
    if (!ftp_native_list_directory(host, port, user, pass, remote_dir, &browser)) {
        util_log(SEVERITY_INFO,
                 "Cleanup(native): list failed or empty for %s/ (already clean?).",
                 remote_dir);
        return true; /* best-effort like WinSCP path */
    }

    for (i = 0; i < browser.count; i++) {
        const char *name = browser.entries[i].name;
        int d = 0;

        if (browser.entries[i].is_directory)
            continue;
        while (name[d] >= '0' && name[d] <= '9')
            d++;
        /* Require at least 3 digits then "__" (WinSCP mask [0-9][0-9][0-9]__*). */
        if (d < 3 || name[d] != '_' || name[d + 1] != '_')
            continue;

        if (native_delete_remote_file(host, port, user, pass,
                                      browser.entries[i].full_path)) {
            removed++;
        }
    }

    util_log(SEVERITY_INFO, "Cleanup(native): removed %d artifact file(s) from %s/",
             removed, remote_dir);
    return true;
}

bool ftp_native_verify_uploads(const char *host, int port, const char *user, const char *pass,
                               const char **local_paths, const char **remote_paths, int count)
{
    int i;
    int missing = 0;
    int size_mismatch = 0;
    int verified = 0;
    int corrupt_count = 0;

    if (!host || !user || !pass || !local_paths || !remote_paths || count <= 0)
        return false;

    util_log(SEVERITY_INFO,
             "Upload integrity check (native): verifying %d file(s) on server...",
             count);

    for (i = 0; i < count; i++) {
        curl_off_t remote_sz;
        curl_off_t local_sz;
        char abs_local[MAX_PATH_LEN];
        const char *basename;

        if (native_is_cancelled())
            return false;
        if (!remote_paths[i] || !local_paths[i])
            continue;

        remote_sz = native_remote_file_size(host, port, user, pass, remote_paths[i]);
        if (remote_sz < 0) {
            util_log(SEVERITY_ERROR, "  MISSING remote: '%s'", remote_paths[i]);
            missing++;
            continue;
        }

        resolve_abs_local(local_paths[i], abs_local, sizeof(abs_local));
        local_sz = local_file_size(abs_local);
        if (local_sz < 0) {
            util_log(SEVERITY_ERROR, "  MISSING local: '%s'", local_paths[i]);
            missing++;
            continue;
        }
        if (local_sz == 0) {
            util_log(SEVERITY_ERROR, "  CORRUPT: '%s' is 0 bytes!", local_paths[i]);
            size_mismatch++;
            continue;
        }
        if (local_sz != remote_sz) {
            util_log(SEVERITY_ERROR,
                     "  SIZE MISMATCH: '%s' local=%lld remote=%lld",
                     remote_paths[i], (long long)local_sz, (long long)remote_sz);
            size_mismatch++;
            continue;
        }

        basename = strrchr(local_paths[i], '\\');
        if (!basename)
            basename = strrchr(local_paths[i], '/');
        if (basename)
            basename++;
        else
            basename = local_paths[i];
        {
            int d = 0;
            while (basename[d] >= '0' && basename[d] <= '9')
                d++;
            if (d >= 2 && basename[d] == '_') {
                util_log(SEVERITY_ERROR,
                         "  ARTIFACT: '%s' has numbered artifact filename!",
                         basename);
                corrupt_count++;
            }
        }

        verified++;
    }

    if (missing > 0 || size_mismatch > 0) {
        util_log(SEVERITY_ERROR,
                 "Upload integrity FAILED (native): missing=%d size_mismatch=%d verified=%d/%d",
                 missing, size_mismatch, verified, count);
        return false;
    }
    if (corrupt_count > 0) {
        util_log(SEVERITY_ERROR,
                 "Upload integrity WARNING (native): %d file(s) have artifact filenames.",
                 corrupt_count);
        return false;
    }

    util_log(SEVERITY_INFO,
             "Upload integrity OK (native): all %d files verified on server.", verified);
    return verified > 0 || count == 0;
}

static void native_sanitize_remote_path_filename(const char *remote_path,
                                                 char *out, size_t out_len)
{
    size_t n = 0;
    const char *p;
    if (!remote_path || !out || out_len == 0)
        return;
    for (p = remote_path; *p && n < out_len - 1; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.')
            out[n++] = (char)ch;
        else
            out[n++] = '_';
    }
    if (n == 0 && out_len > 4) {
        out[n++] = 'f';
        out[n++] = 'i';
        out[n++] = 'l';
        out[n++] = 'e';
    }
    out[n] = '\0';
}

bool ftp_native_create_restore_point(const char *host, int port, const char *user, const char *pass,
                                     const char **remote_paths, int count,
                                     const char *backup_dir, const char *manifest_path)
{
    char (*local_backups)[MAX_PATH_LEN];
    bool *valid;
    int i;
    int valid_count = 0;
    int success_count = 0;
    int fail_count = 0;
    FILE *manifest;
    char ts[64] = {0};

    if (!host || !user || !pass || !remote_paths || count <= 0 ||
        !backup_dir || !manifest_path) {
        return false;
    }

    util_ensure_directory(backup_dir);

    local_backups = (char (*)[MAX_PATH_LEN])calloc((size_t)count, MAX_PATH_LEN);
    valid = (bool *)calloc((size_t)count, sizeof(bool));
    if (!local_backups || !valid) {
        util_log(SEVERITY_ERROR, "Restore point(native): Memory allocation failed.");
        free(local_backups);
        free(valid);
        return false;
    }

    for (i = 0; i < count; i++) {
        char safe_name[384];
        if (!remote_paths[i] || !remote_paths[i][0]) {
            valid[i] = false;
            continue;
        }
        native_sanitize_remote_path_filename(remote_paths[i], safe_name, sizeof(safe_name));
        snprintf(local_backups[i], MAX_PATH_LEN, "%s/%03d_%s",
                 backup_dir, i + 1, safe_name);
        ensure_local_parent_dir(local_backups[i]);
        valid[i] = true;
        valid_count++;
    }

    if (valid_count == 0) {
        util_log(SEVERITY_ERROR, "Restore point(native): No valid remote paths to backup.");
        free(local_backups);
        free(valid);
        return false;
    }

    util_log(SEVERITY_INFO,
             "Restore point(native): Downloading %d file(s)...", valid_count);

    for (i = 0; i < count; i++) {
        if (!valid[i])
            continue;
        if (native_is_cancelled())
            break;
        /* Best-effort: missing remote files are OK (new targets). */
        if (ftp_native_download_file(host, port, user, pass,
                                     remote_paths[i], local_backups[i])) {
            /* ok */
        } else {
            /* download_file removes partials on failure — leave gap for manifest */
        }
    }

    manifest = fopen(manifest_path, "w");
    if (!manifest) {
        util_log(SEVERITY_ERROR, "Restore point(native): Failed to create manifest '%s'",
                 manifest_path);
        free(local_backups);
        free(valid);
        return false;
    }

    util_timestamp(ts, sizeof(ts));
    fprintf(manifest, "# Stelliferum restore point\n");
    fprintf(manifest, "# Created: %s\n", ts);
    fprintf(manifest, "# Backend: native libcurl\n");
    fprintf(manifest, "# Format: remote_path|local_backup_file\n");

    for (i = 0; i < count; i++) {
        if (!valid[i])
            continue;
        if (util_file_exists(local_backups[i])) {
            fprintf(manifest, "%s|%s\n", remote_paths[i], local_backups[i]);
            success_count++;
        } else {
            util_log(SEVERITY_WARNING,
                     "Restore point(native): Could not snapshot '%s' (file not on server?)",
                     remote_paths[i]);
            fail_count++;
        }
    }
    fclose(manifest);

    free(local_backups);
    free(valid);

    if (success_count == 0) {
        util_log(SEVERITY_ERROR,
                 "Restore point(native): No files captured (%d failed)", fail_count);
        return false;
    }

    util_log(SEVERITY_INFO,
             "Restore point(native): Captured %d file(s), %d skipped/failed. Manifest: %s",
             success_count, fail_count, manifest_path);
    return true;
}

bool ftp_native_restore_from_manifest(const char *host, int port, const char *user, const char *pass,
                                      const char *manifest_path)
{
    FILE *manifest;
    char line[2048];
    int entry_count = 0;
    char (*locals)[MAX_PATH_LEN];
    char (*remotes)[MAX_PATH_LEN];
    const char **local_ptrs;
    const char **remote_ptrs;
    int valid_count = 0;
    int skipped = 0;
    int succeeded = 0;
    int failed = 0;
    bool ok;

    if (!host || !user || !pass || !manifest_path)
        return false;

    manifest = fopen(manifest_path, "r");
    if (!manifest) {
        util_log(SEVERITY_ERROR, "Restore(native): Manifest not found '%s'", manifest_path);
        return false;
    }

    while (fgets(line, sizeof(line), manifest)) {
        util_trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        if (strchr(line, '|'))
            entry_count++;
    }

    if (entry_count == 0) {
        fclose(manifest);
        util_log(SEVERITY_ERROR, "Restore(native): Manifest has no entries.");
        return false;
    }

    locals = (char (*)[MAX_PATH_LEN])calloc((size_t)entry_count, MAX_PATH_LEN);
    remotes = (char (*)[MAX_PATH_LEN])calloc((size_t)entry_count, MAX_PATH_LEN);
    if (!locals || !remotes) {
        fclose(manifest);
        free(locals);
        free(remotes);
        util_log(SEVERITY_ERROR, "Restore(native): Memory allocation failed.");
        return false;
    }

    rewind(manifest);
    while (fgets(line, sizeof(line), manifest)) {
        char *sep;
        const char *remote_path;
        const char *local_path;

        util_trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        sep = strchr(line, '|');
        if (!sep)
            continue;
        *sep = '\0';
        remote_path = line;
        local_path = sep + 1;
        if (!remote_path[0] || !local_path[0])
            continue;
        if (!util_file_exists(local_path)) {
            util_log(SEVERITY_WARNING, "Restore(native): Missing local backup '%s'",
                     local_path);
            skipped++;
            continue;
        }
        strncpy(locals[valid_count], local_path, MAX_PATH_LEN - 1);
        strncpy(remotes[valid_count], remote_path, MAX_PATH_LEN - 1);
        valid_count++;
    }
    fclose(manifest);

    if (valid_count == 0) {
        util_log(SEVERITY_ERROR,
                 "Restore(native): No valid files to restore (%d skipped).", skipped);
        free(locals);
        free(remotes);
        return false;
    }

    local_ptrs = (const char **)malloc(sizeof(const char *) * (size_t)valid_count);
    remote_ptrs = (const char **)malloc(sizeof(const char *) * (size_t)valid_count);
    if (!local_ptrs || !remote_ptrs) {
        free(locals);
        free(remotes);
        free(local_ptrs);
        free(remote_ptrs);
        return false;
    }
    {
        int j;
        for (j = 0; j < valid_count; j++) {
            local_ptrs[j] = locals[j];
            remote_ptrs[j] = remotes[j];
        }
    }

    util_log(SEVERITY_INFO, "Restore(native): Uploading %d file(s)...", valid_count);
    ok = ftp_native_upload_batch(host, port, user, pass,
                                 local_ptrs, remote_ptrs, valid_count,
                                 &succeeded, &failed);

    free(local_ptrs);
    free(remote_ptrs);
    free(locals);
    free(remotes);

    util_log(SEVERITY_INFO, "Restore(native) summary: %d restored, %d failed",
             succeeded, failed + skipped);
    return ok;
}

#else /* !STELLI_USE_LIBCURL ------------------------------------------------ */

void ftp_native_reload_security_config(void)
{
    /* No-op without libcurl; host-key path is native-only. */
}

int ftp_native_protocols_ok(void)
{
    return 0;
}

bool ftp_native_download_file(const char *host, int port, const char *user, const char *pass,
                              const char *remote_path, const char *local_path)
{
    (void)host; (void)port; (void)user; (void)pass;
    (void)remote_path; (void)local_path;
    util_log(SEVERITY_ERROR, "ftp_native_download_file: libcurl not enabled");
    return false;
}

bool ftp_native_upload_file(const char *host, int port, const char *user, const char *pass,
                            const char *local_path, const char *remote_path)
{
    (void)host; (void)port; (void)user; (void)pass;
    (void)local_path; (void)remote_path;
    util_log(SEVERITY_ERROR, "ftp_native_upload_file: libcurl not enabled");
    return false;
}

bool ftp_native_upload_batch(const char *host, int port, const char *user, const char *pass,
                             const char **local_paths, const char **remote_paths, int count,
                             int *out_succeeded, int *out_failed)
{
    (void)host; (void)port; (void)user; (void)pass;
    (void)local_paths; (void)remote_paths; (void)count;
    if (out_succeeded) *out_succeeded = 0;
    if (out_failed) *out_failed = count > 0 ? count : 0;
    util_log(SEVERITY_ERROR, "ftp_native_upload_batch: libcurl not enabled");
    return false;
}

bool ftp_native_download_batch(const char *host, int port, const char *user, const char *pass,
                               const char **remote_paths, const char **local_paths, int count)
{
    (void)host; (void)port; (void)user; (void)pass;
    (void)remote_paths; (void)local_paths; (void)count;
    util_log(SEVERITY_ERROR, "ftp_native_download_batch: libcurl not enabled");
    return false;
}

bool ftp_native_list_directory(const char *host, int port, const char *user, const char *pass,
                               const char *remote_path, RemoteFileBrowser *browser)
{
    (void)host; (void)port; (void)user; (void)pass; (void)remote_path;
    if (browser) {
        browser->count = 0;
        snprintf(browser->error, sizeof(browser->error), "libcurl not enabled");
    }
    return false;
}

bool ftp_native_download_recursive(const char *host, int port, const char *user, const char *pass,
                                   const char *remote_dir, const char *local_dir)
{
    (void)host; (void)port; (void)user; (void)pass; (void)remote_dir; (void)local_dir;
    util_log(SEVERITY_ERROR, "ftp_native_download_recursive: libcurl not enabled");
    return false;
}

bool ftp_native_download_core_files(const char *host, int port, const char *user, const char *pass,
                                    const char *remote_root, const char *local_root)
{
    (void)host; (void)port; (void)user; (void)pass; (void)remote_root; (void)local_root;
    util_log(SEVERITY_ERROR, "ftp_native_download_core_files: libcurl not enabled");
    return false;
}

bool ftp_native_upload_directory(const char *host, int port, const char *user, const char *pass,
                                 const char *local_dir, const char *remote_dir)
{
    (void)host; (void)port; (void)user; (void)pass; (void)local_dir; (void)remote_dir;
    util_log(SEVERITY_ERROR, "ftp_native_upload_directory: libcurl not enabled");
    return false;
}

bool ftp_native_cleanup_remote_dir(const char *host, int port, const char *user, const char *pass,
                                   const char *remote_dir)
{
    (void)host; (void)port; (void)user; (void)pass; (void)remote_dir;
    util_log(SEVERITY_ERROR, "ftp_native_cleanup_remote_dir: libcurl not enabled");
    return false;
}

bool ftp_native_verify_uploads(const char *host, int port, const char *user, const char *pass,
                               const char **local_paths, const char **remote_paths, int count)
{
    (void)host; (void)port; (void)user; (void)pass;
    (void)local_paths; (void)remote_paths; (void)count;
    util_log(SEVERITY_ERROR, "ftp_native_verify_uploads: libcurl not enabled");
    return false;
}

bool ftp_native_create_restore_point(const char *host, int port, const char *user, const char *pass,
                                     const char **remote_paths, int count,
                                     const char *backup_dir, const char *manifest_path)
{
    (void)host; (void)port; (void)user; (void)pass; (void)remote_paths; (void)count;
    (void)backup_dir; (void)manifest_path;
    util_log(SEVERITY_ERROR, "ftp_native_create_restore_point: libcurl not enabled");
    return false;
}

bool ftp_native_restore_from_manifest(const char *host, int port, const char *user, const char *pass,
                                      const char *manifest_path)
{
    (void)host; (void)port; (void)user; (void)pass; (void)manifest_path;
    util_log(SEVERITY_ERROR, "ftp_native_restore_from_manifest: libcurl not enabled");
    return false;
}

#endif /* STELLI_USE_LIBCURL */
