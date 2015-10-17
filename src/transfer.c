/*
    This file is a part of saldl.

    Copyright (C) 2014-2015 Mohammad AlSaleh <CE.Mohammad.AlSaleh at gmail.com>
    https://saldl.github.io

    saldl is free software: you can redistribute it and/or modify
    it under the terms of the Affero GNU General Public License as
    published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Affero GNU General Public License for more details.

    You should have received a copy of the Affero GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "events.h"
#include "utime.h"

#define MAX_SEMI_FATAL_RETRIES 5

#ifndef HAVE_STRCASESTR
#include "gnulib_strcasestr.h" // gnulib implementation
#endif

static void set_inline_cookies(CURL *handle, char *cookie_str) {
  char *copy_cookie_str = saldl_strdup(cookie_str);
  char *curr = copy_cookie_str, *next = NULL, *sep = NULL, *cookie = NULL;
  do {
    sep = strstr(curr, ";");
    if (sep) {
      next = sep + 1;
      // space
      sep[0] = '\0';
    }
#ifdef HAVE_ASPRINTF
    SALDL_ASSERT(-1 != asprintf(&cookie, "Set-Cookie: %s; ", curr));
#else
    {
      size_t cookie_len = strlen("Set-Cookie: ") + strlen(curr) + strlen("; ") + 1;
      cookie = saldl_malloc(cookie_len);
    }
#endif
    curl_easy_setopt(handle, CURLOPT_COOKIELIST, cookie);
    SALDL_FREE(cookie);
    curr = next;
  } while (sep);

  SALDL_FREE(copy_cookie_str);
}

char* saldl_user_agent() {
  char *agent = saldl_calloc(1024, sizeof(char));
  snprintf(agent, 1024, "%s/%s", "libcurl", curl_version_info(CURLVERSION_NOW)->version);
  return agent;
}

void chunks_init(info_s *info_ptr) {
  size_t chunk_count = info_ptr->chunk_count;

  info_ptr->chunks = saldl_calloc(chunk_count, sizeof(chunk_s));

  for (size_t idx = 0; idx < chunk_count; idx++) {

    info_ptr->chunks[idx].idx = idx;

    /* size & ranges */
    info_ptr->chunks[idx].size = info_ptr->params->chunk_size;
    info_ptr->chunks[idx].range_start = (off_t)idx * info_ptr->chunks[idx].size;
    info_ptr->chunks[idx].range_end = (off_t)(idx+1) * info_ptr->chunks[idx].size - 1;
  }

  if (info_ptr->rem_size) {
    /* fix size and range_end of the last chunk */
    size_t idx = info_ptr->chunk_count - 1;
    info_ptr->chunks[idx].size = info_ptr->rem_size;
    info_ptr->chunks[idx].range_end = info_ptr->file_size - 1;
  }

  for (size_t idx = 0; idx < chunk_count; idx++) {
    /* events */
    info_ptr->chunks[idx].ev_trigger = &info_ptr->ev_trigger;
    info_ptr->chunks[idx].ev_merge = &info_ptr->ev_merge;
    info_ptr->chunks[idx].ev_queue = &info_ptr->ev_queue;
    info_ptr->chunks[idx].ev_ctrl = &info_ptr->ev_ctrl;
    info_ptr->chunks[idx].ev_status = &info_ptr->ev_status;
  }

}

static void headers_info(info_s *info_ptr) {
  headers_s *h = &info_ptr->headers;

  if (h->location) {
    debug_msg(FN, "Location: %s", h->location);

    if (info_ptr->curr_url) {
      debug_msg(FN, "curr_url was %s", info_ptr->curr_url);
      SALDL_FREE(info_ptr->curr_url);
    }

    info_ptr->curr_url = saldl_strdup(h->location);
  }

  if (h->content_range) {
    char *tmp;
    debug_msg(FN, "Content-Range: %s", h->content_range);

    if ( (tmp = strcasestr(h->content_range, "/")) ) {
      info_ptr->file_size = parse_num_o(tmp+1, 0);
      debug_msg(FN, "file size from Content-Range: %"SAL_JU"", info_ptr->file_size);
    }

    SALDL_FREE(h->content_range);
  }

  if (h->content_encoding) {
    debug_msg(FN, "Content-Encoding: %s", h->content_encoding);
    info_ptr->content_encoded = true;
    SALDL_FREE(h->content_encoding);
  }

  if (h->content_type) {
    /* XXX: content_type block should always be after content_encoding block */
    debug_msg(FN, "Content-Type: %s", h->content_type);

    if (info_ptr->content_type) {
      debug_msg(FN, "Clearing Content-Type: %s", info_ptr->content_type);
      SALDL_FREE(info_ptr->content_type);
    }

    info_ptr->content_type = saldl_strdup(h->content_type);

    if (strcasestr(h->content_type, "gzip")) {
      if (!info_ptr->params->no_compress) {
        debug_msg(FN, "Skipping compression request, the content is already gzipped.");
        info_ptr->params->no_compress = true;
      }

      info_ptr->content_encoded = false;
      /* XXX: no_decompress in case Content-Encoding was forced anyway */
      info_ptr->params->no_decompress = true;
    }

    SALDL_FREE(h->content_type);
  }

  if (h->content_disposition && !info_ptr->params->no_attachment_detection) {
    char *tmp;
    debug_msg(FN, "Content-Disposition: %s", h->content_disposition);

    /* We assume attachment filename is the last assignment */
    if ( (tmp = strrchr(h->content_disposition, '=')) ) {
      tmp++;

      /* Strip ';' if present at the end */
      if (tmp[strlen(tmp) - 1] == ';' ) {
        tmp[strlen(tmp) - 1] = '\0';
      }

      /* Strip quotes if they are present */
      if (*tmp == '"') {
        char *end = strrchr(tmp, '"');
        if (end && (size_t)(end - tmp + 1) == strlen(tmp) ) {
          debug_msg(FN, "Stripping quotes from %s", tmp);
          tmp++;
          *end = '\0';
        }
      }

      /* Strip UTF-8'' if tmp starts with it, this usually comes from filename*=UTF-8''string */
      const char *to_strip = "UTF-8''";
      if (strcasestr(tmp, to_strip) == tmp) {
        tmp += strlen(to_strip);
      }

      if (info_ptr->params->attachment_filename) {
        debug_msg(FN, "Clearing attachment filename: %s", info_ptr->params->attachment_filename);
        SALDL_FREE(info_ptr->content_type);
      }

      debug_msg(FN, "Before basename: %s", tmp);
      info_ptr->params->attachment_filename = saldl_strdup( basename(tmp) );
      debug_msg(FN, "After basename: %s", info_ptr->params->attachment_filename);
    }

    SALDL_FREE(h->content_disposition);
  }

  if (info_ptr->content_encoded && !info_ptr->params->no_decompress) {
      debug_msg(FN, "Content compressed and will be decompressed.");
      debug_msg(FN, "Strict downloaded file size checking will be skipped.");
    }

}

static size_t  header_function(  void  *ptr,  size_t  size, size_t nmemb, void *userdata) {
  headers_s *h = userdata;

  char *header = saldl_strdup(ptr);

  /* Strip \r\n */
  char *tmp;
  if ( (tmp = strstr(header, "\r\n")) ) {
    *tmp = '\0';
  }

  if (strcasestr(header, "Location:") == header) {
    char *h_info = saldl_lstrip(header + strlen("Location:"));
    SALDL_FREE(h->location);
    h->location = saldl_strdup(h_info);
  }

  if (strcasestr(header, "Content-Range:") == header) {
    char *h_info = saldl_lstrip(header + strlen("Content-Range:"));
    SALDL_FREE(h->content_range);
    h->content_range = saldl_strdup(h_info);
  }

  if (strcasestr(header, "Content-Encoding:") == header) {
    char *h_info = saldl_lstrip(header + strlen("Content-Encoding:"));
    SALDL_FREE(h->content_encoding);
    h->content_encoding = saldl_strdup(h_info);
  }

  if (strcasestr(header, "Content-Type:") == header) {
    char *h_info = saldl_lstrip(header + strlen("Content-Type:"));
    SALDL_FREE(h->content_type);
    h->content_type = saldl_strdup(h_info);
  }

  if (strcasestr(header, "Content-Disposition:") == header ) {
    char *h_info = saldl_lstrip(header + strlen("Content-Disposition:"));
    SALDL_FREE(h->content_disposition);
    h->content_disposition = saldl_strdup(h_info);
  }

  SALDL_FREE(header);
  return size * nmemb;
}

static long num_redirects(CURL *handle) {
  long redirects = 0;
  SALDL_ASSERT(handle);
  curl_easy_getinfo(handle, CURLINFO_REDIRECT_COUNT, &redirects);
  return redirects;
}

static void request_remote_info_simple(thread_s *tmp) {
  long response;
  short semi_fatal_retries = 0;
  CURLcode ret;

  /* Disable ranges */
  curl_easy_setopt(tmp->ehandle, CURLOPT_RANGE, NULL);

semi_fatal_request_retry:
  ret = curl_easy_perform(tmp->ehandle);
  debug_msg(FN, "ret=%u", ret);

  switch (ret) {
    case CURLE_OK:
    case CURLE_WRITE_ERROR: /* Caused by *_write_function() returning 0 */
      break;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SEND_ERROR: // 55: SSL_write() returned SYSCALL, errno = 32
      semi_fatal_retries++;
      if (semi_fatal_retries <= MAX_SEMI_FATAL_RETRIES) {
        info_msg(FN, "libcurl returned semi-fatal (%d: %s), retry %d/%d",
            ret, tmp->err_buf,
            semi_fatal_retries, MAX_SEMI_FATAL_RETRIES);
        goto semi_fatal_request_retry;
      }
      else {
        fatal(FN, "libcurl returned semi-fatal (%d: %s). max retries(%d) exceeded.",
            ret, tmp->err_buf,
            MAX_SEMI_FATAL_RETRIES);
      }
      break;
    default:
      fatal(FN, "libcurl returned (%d: %s) while trying to get remote info.", ret, tmp->err_buf);
  }

  curl_easy_getinfo(tmp->ehandle, CURLINFO_RESPONSE_CODE, &response);
  debug_msg(FN, "response=%ld", response );
}

static off_t remote_info_simple_file_size(CURL *handle) {
  double d_size;
  off_t size = 0;

  curl_easy_getinfo(handle,CURLINFO_CONTENT_LENGTH_DOWNLOAD,&d_size);

  size = (off_t)d_size;
  debug_msg(FN, "filesize=%"SAL_JD"", size);

  if ( size == -1 ) {
    debug_msg(FN, "Zeroing filesize, was -1.");
    size = 0;
  }

  return size;
}

static int request_remote_info_with_ranges(thread_s *tmp, info_s *info_ptr) {
  CURLcode ret;
  saldl_params *params_ptr = info_ptr->params;

  short semi_fatal_retries = 0;
  bool semi_fatal_error = false;

  const double expected_length = 4096;
  double content_length = 0;

  if (params_ptr->assume_range_support) {
    debug_msg(FN, "Range support assumed, skipping check.");
    return 0;
  }

  if (params_ptr->single_mode && !params_ptr->resume) {
    debug_msg(FN, "Skipping unnecessary range check.");
    goto no_range;
  }

  debug_msg(FN, "Checking server response with range support..");
  curl_easy_setopt(tmp->ehandle, CURLOPT_RANGE, "4096-8191");

  while (semi_fatal_retries <= MAX_SEMI_FATAL_RETRIES) {
    ret = curl_easy_perform(tmp->ehandle);
    semi_fatal_error = (ret == CURLE_SSL_CONNECT_ERROR || ret == CURLE_SEND_ERROR);

    if (!semi_fatal_error) {
      break;
    }
    else {
      semi_fatal_retries++;
      warn_msg(FN, "libcurl returned semi-fatal error (%d: %s), retry %d/%d.", ret, tmp->err_buf, semi_fatal_retries, MAX_SEMI_FATAL_RETRIES);
    }

  }

  curl_easy_getinfo(tmp->ehandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_length);

  if (content_length == expected_length) {

    /* Special handling for FTP */
    if (strstr(info_ptr->curr_url, "ftp") == info_ptr->curr_url) {
      /*
       * Despite the fact that range support is working. Making
       * concurrent connections to FTP servers seems problematic.
       * So, we force single mode, but allow resume if requested.
       */
      warn_msg(FN, "Range support works. But we force single mode for ftp URLs.");
      params_ptr->single_mode = true;
    }

    /* Return success */
    return 0;
  }
  /* Range check failed */
  else {
    warn_msg(FN, "Wrong link, server lacks range support or file too small.");
    warn_msg(FN, "We well make a second check without ranges.");
    debug_msg(FN, "Expected length %.0lf, got %.0lf", expected_length, content_length);
  }

no_range:
  warn_msg(FN, "Single mode force-enabled, resume force-disabled.");
  params_ptr->single_mode = true;
  params_ptr->resume = false;
  return -1;

}

static void set_names(info_s* info_ptr) {

  saldl_params *params_ptr = info_ptr->params;

  if (!params_ptr->filename) {
    char *prev_unescaped, *unescaped;

    /* curl_easy_unescape() does not require an initialized handle */
    CURL *handle = NULL;

    /* Get initial filename */
    if (params_ptr->attachment_filename) {
      prev_unescaped = saldl_strdup(params_ptr->attachment_filename);
    }
    else if (params_ptr->filename_from_redirect) {
      prev_unescaped = saldl_strdup(info_ptr->curr_url);
    }
    else {
      prev_unescaped = saldl_strdup(params_ptr->start_url);
    }

    /* unescape name/url */
    void(*free_prev)(void*) = free;
    while ( strcmp(unescaped = curl_easy_unescape(handle, prev_unescaped, 0, NULL), prev_unescaped) ) {
      curl_free(prev_unescaped);
      prev_unescaped = unescaped;
      free_prev = curl_free;
    }
    free_prev(prev_unescaped);

    /* keep attachment name ,if present, as is. basename() unescaped url */
    if (params_ptr->attachment_filename) {
      params_ptr->filename = saldl_strdup(unescaped);
    } else {
      params_ptr->filename = saldl_strdup(basename(unescaped));
    }
    curl_free(unescaped);

    /* Finally, remove GET atrrs if present */
    if (!params_ptr->keep_GET_attrs) {
      char *pre_filename = saldl_strdup(params_ptr->filename);
      char *q = strrchr(params_ptr->filename, '?');

      /* Only strip if we're not going to end up with an empty filename */
      if (q && q != params_ptr->filename && *(q-1) != '/') {
        if (strchr(q, '=')) {
          q[0] = '\0';
        }
      }

      if ( strcmp(pre_filename, params_ptr->filename) ) {
        info_msg(FN, "Before stripping GET attrs: %s", pre_filename);
        info_msg(FN, "After  stripping GET attrs: %s", params_ptr->filename);
      }
      SALDL_FREE(pre_filename);
    }

  }

  if ( !strcmp(params_ptr->filename,"") || !strcmp(params_ptr->filename,".") || !strcmp(params_ptr->filename,"..") || !strcmp(params_ptr->filename,"...") || ( strrchr(params_ptr->filename,'/') && ( !strcmp(strrchr(params_ptr->filename,'/'), "/") || !strcmp(strrchr(params_ptr->filename,'/'), "/.") || !strcmp(strrchr(params_ptr->filename,'/'), "/..") ) ) ) {
    fatal(NULL, "Invalid filename \"%s\".", params_ptr->filename);
  }

  if ( params_ptr->no_path ) {
    if ( strchr(params_ptr->filename, '/') ) {
      info_msg(FN, "Replacing %s/%s with %s_%s in %s", error_color, end, ok_color, end, params_ptr->filename);
    }
    params_ptr->filename = valid_filename(params_ptr->filename);
  }

  /* Prepend dir if set */
  if (params_ptr->root_dir) {

    /* strip '/' if present at the end of root_dir */
    if (params_ptr->root_dir[strlen(params_ptr->root_dir)-1] == '/') {
      params_ptr->root_dir[strlen(params_ptr->root_dir)-1] = '\0';
    }

    char *curr_filename = params_ptr->filename;
    size_t full_buf_size = strlen(params_ptr->root_dir) + strlen(curr_filename) + 2; // +1 for '/', +1 for '\0'

    info_msg(FN, "Prepending root_dir(%s) to filename(%s).", params_ptr->root_dir, params_ptr->filename);
    params_ptr->filename = saldl_calloc(full_buf_size, sizeof(char)); // +1 for '\0'
    snprintf(params_ptr->filename, full_buf_size, "%s/%s", params_ptr->root_dir, curr_filename);
    SALDL_FREE(curr_filename);
  }

  if (params_ptr->auto_trunc || params_ptr->smart_trunc) {
    char *before_trunc = params_ptr->filename;

    /* See if smart_trunc is enabled before auto_trunc */
    if ( params_ptr->smart_trunc ) {
      params_ptr->filename = trunc_filename( params_ptr->filename, 1);
    }

    if ( params_ptr->auto_trunc ) {
      params_ptr->filename = trunc_filename( params_ptr->filename, 0);
    }

    if ( strlen(before_trunc) != strlen(params_ptr->filename) ) {
      warn_msg(NULL,"Filename truncated:");
      warn_msg(NULL,"  Original: %s", before_trunc);
      warn_msg(NULL,"  Truncated: %s", params_ptr->filename);
    }
  }

  /* Check if filename exists  */
  if (!access(params_ptr->filename,F_OK)) {
    fatal(NULL, "%s exists, quiting...", params_ptr->filename);
  }

  /* Set part/ctrl filenames, tmp dir */
  {
    char cwd[PATH_MAX];
    snprintf(info_ptr->part_filename,PATH_MAX-(params_ptr->filename[0]=='/'?0:strlen(getcwd(cwd,PATH_MAX))+1),"%s.part.sal",params_ptr->filename);
    snprintf(info_ptr->ctrl_filename,PATH_MAX,"%s.ctrl.sal",params_ptr->filename);
    snprintf(info_ptr->tmp_dirname,PATH_MAX,"%s.tmp.sal",params_ptr->filename);
  }
}

static void print_info(info_s *info_ptr) {
  saldl_params *params_ptr = info_ptr->params;

  main_msg("URL", params_ptr->start_url);

  if (strcmp(params_ptr->start_url, info_ptr->curr_url)) {
    main_msg("Redirected", info_ptr->curr_url);
  }

  if (info_ptr->content_type) {
    main_msg("Content-Type", info_ptr->content_type);
  }

  main_msg("Saving To", params_ptr->filename);

  if (info_ptr->file_size > 0) {
    off_t file_size = info_ptr->file_size;
    main_msg("File Size", "%.2f%s", human_size(file_size), human_size_suffix(file_size));
  }
}

void get_info(info_s *info_ptr) {
  saldl_params *params_ptr = info_ptr->params;
  thread_s tmp = {0};

  if (params_ptr->no_remote_info) {
    warn_msg(FN, "no_remote_info enforces both enabling single mode and disabling resume.");
    params_ptr->single_mode = true;
    params_ptr->resume = false;
    goto no_remote;
  }

  /* remote part starts here */
  tmp.ehandle = curl_easy_init();
  set_params(&tmp, info_ptr);

  if (!params_ptr->no_timeouts) {
    /* Resolving the host for the 1st time takes a long time sometimes */
    curl_easy_setopt(tmp.ehandle, CURLOPT_LOW_SPEED_TIME, 75l);
  }

  curl_easy_setopt(tmp.ehandle, CURLOPT_HEADERFUNCTION, header_function);
  curl_easy_setopt(tmp.ehandle, CURLOPT_HEADERDATA, &info_ptr->headers);

  if (params_ptr->head && !params_ptr->post && !params_ptr->raw_post) {
    info_msg(FN, "Using HEAD for remote info.");
    curl_easy_setopt(tmp.ehandle,CURLOPT_NOBODY,1l);
  }

  set_write_opts(tmp.ehandle, NULL, params_ptr, true);

  /*
   * Check remote info with range support in one request.
   * If that check fails in a way we're not expecting, do
   * a secondary check without ranges.
   * We also make a 2nd check if filesize was not set. This
   * could happen with non-HTTP protocols like FTP.
   */
  int ret_ranges = request_remote_info_with_ranges(&tmp, info_ptr);
  headers_info(info_ptr);

  if (ret_ranges || !info_ptr->file_size) {
    request_remote_info_simple(&tmp);
    headers_info(info_ptr);
    /* We didn't get file size from Content-Range, so get it from Content-Length */
    info_ptr->file_size = remote_info_simple_file_size(tmp.ehandle);
  }

  curl_easy_cleanup(tmp.ehandle);
  /* remote part ends here */

no_remote:
  set_names(info_ptr);
  print_info(info_ptr);

}

void check_remote_file_size(info_s *info_ptr) {

  saldl_params *params_ptr = info_ptr->params;

  if (info_ptr->chunk_count <= 1 || params_ptr->single_mode) {
    set_single_mode(info_ptr);
  }

  if (info_ptr->chunk_count > 1 && info_ptr->chunk_count < info_ptr->params->num_connections) {
    info_msg(NULL, "File relatively small, use %"SAL_ZU" connection(s)", info_ptr->chunk_count);
    info_ptr->params->num_connections = info_ptr->chunk_count;
  }
}

static void whole_file(info_s *info_ptr) {
  if (0 < info_ptr->file_size) {
    info_ptr->params->chunk_size = saldl_max_z_umax((uintmax_t)info_ptr->params->chunk_size , (uintmax_t)info_ptr->file_size  / info_ptr->params->num_connections);
    info_ptr->params->chunk_size = info_ptr->params->chunk_size >> 12 << 12 ; /* Round down to 4k boundary */
    info_msg(FN, "Chunk size set to %.2f%s based on file size %.2f%s and number of connections %"SAL_ZU".",
        human_size(info_ptr->params->chunk_size), human_size_suffix(info_ptr->params->chunk_size),
        human_size(info_ptr->file_size), human_size_suffix(info_ptr->file_size),
        info_ptr->params->num_connections);
  }
}

static void auto_size_func(info_s *info_ptr, int auto_size) {
  int cols = tty_width();
  if (cols <= 0) {
    info_msg(NULL, "Couldn't retrieve tty width. Chunk size will not be modified.");
    return;
  }

  if (cols <= 2) {
    info_msg(NULL, "Retrieved tty width (%d) is too small.", cols);
    return;
  }

  if (0 < info_ptr->file_size) {
    size_t orig_chunk_size = info_ptr->params->chunk_size;

    if ( info_ptr->params->num_connections > (size_t)cols ) {
      info_ptr->params->num_connections = (size_t)cols; /* Limit active connections to 1 line */
      info_msg(NULL, "no. of connections reduced to %"SAL_ZU" based on tty width %d.", info_ptr->params->num_connections, cols);
    }

    if ( ( info_ptr->params->chunk_size = saldl_max_z_umax((uintmax_t)orig_chunk_size, (uintmax_t)info_ptr->file_size / (uintmax_t)(cols * auto_size) ) ) != orig_chunk_size) {
      info_ptr->params->chunk_size = (info_ptr->params->chunk_size  + (1<<12) - 1) >> 12 << 12; /* Round up to 4k boundary */
      info_msg(FN, "Chunk size set to %.2f%s, no. of connections set to %"SAL_ZU", based on tty width %d and no. of lines requested %d.",
          human_size(info_ptr->params->chunk_size), human_size_suffix(info_ptr->params->chunk_size),
          info_ptr->params->num_connections, cols, auto_size);
    }

  }

}

void check_url(char *url) {
  /* TODO: Add more checks */
  if (! strcmp(url, "") ) {
    fatal(NULL, "Invalid empty url \"%s\".", url);
  }
}

void set_info(info_s *info_ptr) {

  saldl_params *params_ptr = info_ptr->params;

  /* I know this is a crazy way to set defaults */
  params_ptr->num_connections += !params_ptr->num_connections * SALDL_DEF_NUM_CONNECTIONS;
  params_ptr->chunk_size += !params_ptr->chunk_size * SALDL_DEF_CHUNK_SIZE;

  if (! params_ptr->single_mode) {
    if ( params_ptr->auto_size  ) {
      auto_size_func(info_ptr, params_ptr->auto_size);
    }

    if ( params_ptr->whole_file  ) {
      whole_file(info_ptr);
    }
  }

  /* Chunk size should be at least 4k */
  if (info_ptr->params->chunk_size < 4096) {
    warn_msg(FN, "Rounding up chunk_size from %"SAL_ZU" to 4096(4k).", info_ptr->params->chunk_size);
    info_ptr->params->chunk_size = 4096;
  }

  info_ptr->rem_size = (size_t)(info_ptr->file_size % (off_t)info_ptr->params->chunk_size);
  info_ptr->chunk_count = (size_t)(info_ptr->file_size / (off_t)info_ptr->params->chunk_size) + !!info_ptr->rem_size;

}

void print_chunk_info(info_s *info_ptr) {
  if (info_ptr->file_size) { /* Avoid printing useless info if remote file size is reported 0 */

    size_t chunk_size = info_ptr->params->chunk_size;
    if (info_ptr->rem_size && !info_ptr->params->single_mode) {
      main_msg("Chunks", "%"SAL_ZU"*%.2f%s + 1*%.2f%s", info_ptr->chunk_count-1,
          human_size(chunk_size), human_size_suffix(chunk_size),
          human_size(info_ptr->rem_size), human_size_suffix(info_ptr->rem_size));
    } else {
      main_msg("Chunks", "%"SAL_ZU"*%.2f%s", info_ptr->chunk_count,
          human_size(chunk_size), human_size_suffix(chunk_size));
    }
  }

}

void global_progress_init(info_s *info_ptr) {
  info_ptr->global_progress.start = saldl_utime();
  info_ptr->global_progress.curr = info_ptr->global_progress.prev = info_ptr->global_progress.start;
  info_ptr->global_progress.initialized = 1;
}

void global_progress_update(info_s *info_ptr, bool init) {
  progress_s *p = &info_ptr->global_progress;
  chunks_progress_s *chsp = &p->chunks_progress;
  if (info_ptr->chunks) {
    size_t idx;
    off_t total_complete_size = 0;
    size_t merged = 0;
    size_t finished = 0;
    size_t started = 0;
    size_t empty_started = 0;
    size_t queued = 0;
    size_t not_started = 0;
    for (idx = 0; idx < info_ptr->chunk_count; idx++) {
      chunk_s chunk = info_ptr->chunks[idx]; /* Important to get consistent info */
      total_complete_size += chunk.size_complete;

      /* Status */
      switch (chunk.progress) {
        case PRG_MERGED:
          merged++;
          break;
        case PRG_FINISHED:
          finished++;
        case PRG_STARTED:
          started++;
          if (!chunk.size_complete) {
            empty_started++;
          }
          break;
        case PRG_QUEUED:
          queued++;
        case PRG_NOT_STARTED:
          not_started++;
          break;
        default:
          fatal(FN, "Invalid progress value %d for chunk %"SAL_ZU"", chunk.progress, idx);
          break;
      }
    }

    /* Apply update */
    p->complete_size = total_complete_size;
    chsp->merged = merged;
    chsp->finished = finished;
    chsp->started = started;
    chsp->empty_started = empty_started;
    chsp->queued = queued;
    chsp->not_started = not_started;
  }

  if (init) {
    p->initial_complete_size = p->complete_size;
    p->dlprev = p->complete_size;
  }
}

static int status_single_display(void *void_info_ptr, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {

  uintmax_t lines = 5;
  info_s *info_ptr = (info_s *)void_info_ptr;
  saldl_params *params_ptr = info_ptr->params;

  double params_refresh = params_ptr->status_refresh_interval;
  double refresh_interval = params_refresh ? params_refresh : SALDL_DEF_STATUS_REFRESH_INTERVAL;

  SALDL_ASSERT(!ulnow || info_ptr->params->post || info_ptr->params->raw_post);
  SALDL_ASSERT(!ultotal || info_ptr->params->post || info_ptr->params->raw_post);

  if (info_ptr) {
    progress_s *p = &info_ptr->global_progress;
    if (p->initialized) {
      long curr_redirects_count = num_redirects(info_ptr->threads[0].ehandle);

      if (info_ptr->file_size_from_dltotal && curr_redirects_count != info_ptr->redirects_count) {
        debug_msg(FN, "Resetting file_size from dltotal, redirect count changed from %ld to %ld.",
            info_ptr->redirects_count, curr_redirects_count);
        info_ptr->file_size = 0;
      }

      info_ptr->redirects_count = curr_redirects_count;

      if (!info_ptr->file_size && dltotal) {
        debug_msg(FN, "Setting file_size from dltotal=%"SAL_JU".", dltotal);
        info_ptr->file_size = dltotal;
        info_ptr->file_size_from_dltotal = true;
      }

      curl_off_t offset = dltotal && info_ptr->file_size > dltotal ? (curl_off_t)info_ptr->file_size - dltotal : 0;
      info_ptr->chunks[0].size_complete = (size_t)(offset + dlnow);

      /* Return early if no_status, but after setting size_complete */
      if (info_ptr->params->no_status) {
        return 0;
      }

      if (p->initialized == 1) {
        p->initialized++;
        saldl_fputs_count(lines+1, "\n", stderr, "stderr"); // +1 in case offset becomes non-zero
      }

      p->curr = saldl_utime();
      p->dur = p->curr - p->start;
      p->curr_dur = p->curr - p->prev;


      /* Update every refresh_interval, and when the download finishes.
       * Always update when file_size is unknown(dltotal==0), as
       * the download might finish anytime.
       * */
      if (p->curr_dur >= refresh_interval || !dltotal  || dlnow == dltotal) {
        if (p->curr_dur >= refresh_interval) {
          off_t curr_done = saldl_max_o(dlnow + offset - p->dlprev, 0); // Don't go -ve on reconnects
          p->curr_rate =  curr_done / p->curr_dur;
          p->curr_rem = p->curr_rate && dltotal ? (dltotal - dlnow) / p->curr_rate : INT64_MAX;

          p->prev = p->curr;
          p->dlprev = dlnow + offset;
        }

        if (p->dur >= SALDL_STATUS_INITIAL_INTERVAL) {
          p->rate = dlnow / p->dur;
          p->rem = p->rate && dltotal ? (dltotal - dlnow) / p->rate : INT64_MAX;
        }

        saldl_fputs_count(lines+!!offset, up, stderr, "stderr");
        main_msg("Single mode progress", " ");

        status_msg("Progress", " \t%.2f%s / %.2f%s",
            human_size(dlnow + offset), human_size_suffix(dlnow + offset),
            human_size(info_ptr->file_size), human_size_suffix(info_ptr->file_size));

        if (offset) {
          status_msg(NULL, "        \t%.2f%s / %.2f%s (Offset: %.2f%s)",
              human_size(dlnow), human_size_suffix(dlnow),
              human_size(dltotal), human_size_suffix(dltotal),
              human_size(offset), human_size_suffix(offset));
        }

        status_msg("Rate", "     \t%.2f%s/s : %.2f%s/s",
            human_size(p->rate), human_size_suffix(p->rate),
            human_size(p->curr_rate), human_size_suffix(p->curr_rate));

        status_msg("Remaining", "\t%.1fs : %.1fs", p->rem, p->curr_rem);
        status_msg("Duration", " \t%.1fs", p->dur);
      }
    }
  }
  return 0;
}

static int chunk_progress(void *void_chunk_ptr, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {

  SALDL_ASSERT(!ulnow);
  SALDL_ASSERT(!ultotal);

  chunk_s *chunk =  (chunk_s *)void_chunk_ptr;
  size_t rem;

  /* Check bad server behavior, e.g. if dltotal becomes file_size mid-transfer. */
  if (dltotal && dltotal != (chunk->range_end - chunk->curr_range_start + 1) ) {
    fatal(FN, "Transfer size(%"SAL_JD") does not match requested range(%"SAL_JD"-%"SAL_JD") in chunk %"SAL_ZU", this is a sign of a bad server, retry with a single connection.", (intmax_t)dltotal, (intmax_t)chunk->curr_range_start, (intmax_t)chunk->range_end, chunk->idx);
  }

  if (dlnow) { /* dltotal & dlnow can both be 0 initially */
    curl_off_t curr_chunk_size = chunk->range_end - chunk->curr_range_start + 1;
    if (dltotal != curr_chunk_size) {
      fatal(FN, "Transfer size does not equal requested range: %"SAL_JD"!=%"SAL_JD" for chunk %"SAL_ZU", this is a sign of a bad server, retry with a single connection.", (intmax_t)dltotal, (intmax_t)curr_chunk_size, chunk->idx);
    }
    rem = (size_t)(dltotal-dlnow);
  } else if (chunk->size_complete) { /* dltotal & dlnow can also both be 0 initially if a chunk download restarted */
    rem = chunk->size - chunk->size_complete;
  } else {
    rem = chunk->size;
  }
  chunk->size_complete = chunk->size - rem;

  return 0;
}

void set_progress_params(thread_s *thread, info_s *info_ptr) {

  saldl_params *params_ptr = info_ptr->params;

  if (params_ptr->single_mode) {
    curl_easy_setopt(thread->ehandle,CURLOPT_XFERINFOFUNCTION,&status_single_display);
    curl_easy_setopt(thread->ehandle,CURLOPT_XFERINFODATA,info_ptr);
    curl_easy_setopt(thread->ehandle,CURLOPT_NOPROGRESS,0l);
  } else if (thread->chunk && thread->chunk->size) {
    curl_easy_setopt(thread->ehandle,CURLOPT_XFERINFOFUNCTION,&chunk_progress);
    curl_easy_setopt(thread->ehandle,CURLOPT_XFERINFODATA, thread->chunk);
    curl_easy_setopt(thread->ehandle,CURLOPT_NOPROGRESS,0l);
  }
}

void set_params(thread_s *thread, info_s *info_ptr) {
  saldl_params *params_ptr = info_ptr->params;

  curl_easy_setopt(thread->ehandle, CURLOPT_ERRORBUFFER, thread->err_buf);

#if !defined(__CYGWIN__) && !defined(__MSYS__) && defined(HAVE_GETMODULEFILENAME)
  /* Set CA bundle if the file exists */
  char ca_bundle_path[PATH_MAX];
  char *exe_dir = windows_exe_path();
  if (exe_dir) {
    snprintf(ca_bundle_path, PATH_MAX, "%s/ca-bundle.trust.crt", exe_dir);

    if ( access(ca_bundle_path, F_OK) ) {
      warn_msg(FN, "CA bundle file %s does not exist.", ca_bundle_path);
    }
    else {
      curl_easy_setopt(thread->ehandle, CURLOPT_CAINFO, ca_bundle_path);
    }

    SALDL_FREE(exe_dir);
  }
#endif

#if LIBCURL_VERSION_MINOR >= 45
  /* CURLOPT_DEFAULT_PROTOCOL was introduced in libcurl 7.45.0 */
  if (info_ptr->curl_info->version_num >= 0x072d00) { // >= 7.45
    /* If protocol unknown, assume https */
    curl_easy_setopt(thread->ehandle, CURLOPT_DEFAULT_PROTOCOL, "https");
  }
#endif

  if (!params_ptr->no_http2) {
    /* Try HTTP/2, but don't care about the return value.
     * Most libcurl binaries would not include support for HTTP/2 in the short term */
    curl_easy_setopt(thread->ehandle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
  }

  /* For our use-cases, Nagle's algorithm seems to have negative or
   * no impact on performance. libcurl disables the algorithm for
   * HTTP/2 by default now. And I decided to disable it for all
   * protocols here.
   */
  curl_easy_setopt(thread->ehandle, CURLOPT_TCP_NODELAY, 1l);

  /* Send post fields if provided */

  /* If both raw_post & post are set, post is ignored */
  if (params_ptr->raw_post) {
    struct curl_slist *custom_headers = NULL;
    curl_easy_setopt(thread->ehandle, CURLOPT_POST, 1L);

    /* Disable headers set by CURLOPT_POST */
    custom_headers = curl_slist_append(custom_headers, "Content-Type:");
    custom_headers = curl_slist_append(custom_headers, "Content-Length:");

    /* Append raw_post as-is to custom_headers */
    custom_headers = curl_slist_append(custom_headers, params_ptr->raw_post);

    /* Add custom_headers to the request */
    curl_easy_setopt(thread->ehandle, CURLOPT_HTTPHEADER, custom_headers);
  } else if (params_ptr->post) {
    debug_msg(FN, "POST fields: %s", params_ptr->post);
    curl_easy_setopt(thread->ehandle,CURLOPT_POSTFIELDS, params_ptr->post);
  }

  if (params_ptr->cookie_file) {
    curl_easy_setopt(thread->ehandle, CURLOPT_COOKIEFILE, params_ptr->cookie_file);
  } else {
    /* Just enable the cookie engine */
    curl_easy_setopt(thread->ehandle, CURLOPT_COOKIEFILE, "");
  }

  if (params_ptr->inline_cookies) {
    set_inline_cookies(thread->ehandle, params_ptr->inline_cookies);
  }

  curl_easy_setopt(thread->ehandle, CURLOPT_URL, info_ptr->curr_url);

  curl_easy_setopt(thread->ehandle,CURLOPT_NOSIGNAL,1l); /* Try to avoid threading related segfaults */
  curl_easy_setopt(thread->ehandle,CURLOPT_FAILONERROR,1l); /* Fail on 4xx errors */
  curl_easy_setopt(thread->ehandle,CURLOPT_FOLLOWLOCATION,1l); /* Handle redirects */

  if (params_ptr->libcurl_verbosity) {
    debug_msg(FN, "enabling libcurl verbose output.");
    curl_easy_setopt(thread->ehandle,CURLOPT_VERBOSE,1l);
  }

  if (params_ptr->connection_max_rate) {
    curl_easy_setopt(thread->ehandle, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)params_ptr->connection_max_rate);
  }

  if (!params_ptr->no_timeouts) {
    curl_easy_setopt(thread->ehandle,CURLOPT_LOW_SPEED_LIMIT,512l); /* Abort if dl rate goes below .5K/s for > 15 seconds */
    curl_easy_setopt(thread->ehandle,CURLOPT_LOW_SPEED_TIME,10l);
  }

  if (params_ptr->tls_no_verify) {
    curl_easy_setopt(thread->ehandle,CURLOPT_SSL_VERIFYPEER,0l);
    curl_easy_setopt(thread->ehandle,CURLOPT_SSL_VERIFYHOST,0l);
  }

  if (!params_ptr->no_user_agent) {
    if (params_ptr->user_agent) {
      curl_easy_setopt(thread->ehandle, CURLOPT_USERAGENT, params_ptr->user_agent);
    }
    else {
      char *default_agent = saldl_user_agent();
      curl_easy_setopt(thread->ehandle, CURLOPT_USERAGENT, default_agent);
      SALDL_FREE(default_agent);
    }
  }

  /* NOTE: no_compress could be set by the user, or by header_function() */
  if (!params_ptr->no_compress) {
    curl_easy_setopt(thread->ehandle, CURLOPT_ACCEPT_ENCODING, ""); /* "" sends all supported encodings */

    /* We do this here as setting no_decompress with no_compress is meaningless */
    if (params_ptr->no_decompress) {
      curl_easy_setopt(thread->ehandle, CURLOPT_HTTP_CONTENT_DECODING, 0l);
    }
  }

  if (params_ptr->proxy) {
    curl_easy_setopt(thread->ehandle,CURLOPT_PROXY,params_ptr->proxy);
  }
  if (params_ptr->tunnel_proxy) {
    curl_easy_setopt(thread->ehandle,CURLOPT_PROXY,params_ptr->tunnel_proxy);
    curl_easy_setopt(thread->ehandle,CURLOPT_HTTPPROXYTUNNEL,1l);
  }
  if (params_ptr->no_proxy) {
    curl_easy_setopt(thread->ehandle,CURLOPT_PROXY,"");
  }

  if (params_ptr->auto_referer) {
    curl_easy_setopt(thread->ehandle, CURLOPT_AUTOREFERER, 1l);
  }

  if (params_ptr->referer) {
    curl_easy_setopt(thread->ehandle, CURLOPT_REFERER, params_ptr->referer);
  }
}

void set_single_mode(info_s *info_ptr) {

  saldl_params *params_ptr = info_ptr->params;

  if (!params_ptr->single_mode) {
    info_msg(FN, "File small, enabling single mode.");
    params_ptr->single_mode = true;
  }

  /* XXX: Should we try to support large files with single mode in 32b systems? */
  if ((uintmax_t)info_ptr->file_size > (uintmax_t)SIZE_MAX) {
    fatal(FN, "Trying to set single chunk size to file_size %"SAL_JD", but chunk size can't exceed %"SAL_ZU" ", (intmax_t)info_ptr->file_size, SIZE_MAX);
  }

  info_ptr->params->chunk_size = (size_t)info_ptr->file_size;
  info_ptr->chunk_count = info_ptr->params->num_connections = 1;
  info_ptr->rem_size = 0;
}

void check_files_and_dirs(info_s *info_ptr) {
  saldl_params *params_ptr = info_ptr->params;

  if (!params_ptr->read_only) {
    if (params_ptr->resume) {
      if ( !(info_ptr->file = fopen(info_ptr->part_filename,"rb+")) ) {
        fatal(FN, "Failed to open %s for writing: %s", info_ptr->part_filename, strerror(errno));
      }
    }
    else {
      if ( !params_ptr->force  && !access(info_ptr->part_filename,F_OK)) {
        fatal(FN, "%s exists, enable 'resume' or 'force' to overwrite.", info_ptr->part_filename);
      }
      if ( !(info_ptr->file = fopen(info_ptr->part_filename,"wb")) ) {
        fatal(FN, "Failed to open %s for writing: %s", info_ptr->part_filename, strerror(errno));
      }
    }
  }

  /* if tmp dir exists */
  if (! access(info_ptr->tmp_dirname, F_OK) ) {
    if (params_ptr->mem_bufs || params_ptr->single_mode) {
      warn_msg(FN, "%s seems to be left over. You have to delete the dir manually.", info_ptr->tmp_dirname);
    }

    if (!info_ptr->extra_resume_set) {
      fatal(FN, "%s is left over from a previous run with different chunk size. You have to use the same chunk size or delete the dir manually.", info_ptr->tmp_dirname);
    }
  }
  /* if dir does not exist */
  else if (!params_ptr->read_only && !params_ptr->mem_bufs && !params_ptr->single_mode) {
    if ( info_ptr->extra_resume_set ) {
      warn_msg(FN, "%s did not exist. Maybe previous run used memory buffers or the dir was deleted manually.", info_ptr->tmp_dirname);
    }
    /* mkdir with 700 perms */
    if ( saldl_mkdir(info_ptr->tmp_dirname, S_IRWXU) ) {
      fatal(FN, "Failed to create %s: %s", info_ptr->tmp_dirname, strerror(errno) );
    }
  }

  /* ctrl */
  if (!params_ptr->read_only) {
    if (!params_ptr->resume && !params_ptr->force && !access(info_ptr->ctrl_filename, F_OK)) {
      fatal(FN, "Resume disabled and %s exists.", info_ptr->ctrl_filename);
    }

    info_ptr->ctrl_file = fopen(info_ptr->ctrl_filename,"wb+");

    if (!info_ptr->ctrl_file) {
      fatal(FN, "Failed to open '%s' for read/write : %s", info_ptr->ctrl_filename, strerror(errno) );
    }
  }
}

void saldl_perform(thread_s *thread) {
  CURLcode ret;
  long response;
  size_t retries = 0;
  size_t delay = 1;
  short semi_fatal_retries = 0;


  while (1) {

#ifdef HAVE_SIGACTION
    /* shutdown code in OpenSSL sometimes raises SIGPIPE, and libcurl
     * should be already ignoring that signal, but for some reason, it's
     * still raised sometimes, so, we are ignoring it explicitly here */

    struct sigaction sa_orig;
    ignore_sig(SIGPIPE, &sa_orig);
#endif

    ret = curl_easy_perform(thread->ehandle);

#ifdef HAVE_SIGACTION
    /* Restore SIGPIPE handler */
    restore_sig_handler(SIGPIPE, &sa_orig);
#endif

    /* Break if everything went okay */
    if (ret == CURLE_OK && thread->chunk->size_complete == thread->chunk->size) {
      goto saldl_perform_success;
    }

    switch (ret) {
      case CURLE_OK:
        if (thread->chunk->size) {
          if (!thread->chunk->size_complete) {
            /* This happens sometimes, especially when tunneling through proxies! Consider it non-fatal and retry */
            info_msg(FN, "libcurl returned CURLE_OK for chunk %"SAL_ZU" before getting any data , restarting (retry %"SAL_ZU", delay=%"SAL_ZU").", thread->chunk->idx, ++retries, delay);
          }
          else {
            /* Trust libcurl here if single mode */
            if (thread->single) {
              warn_msg(FN, "Returned CURLE_OK, but completed size(%"SAL_ZU") != requested size(%"SAL_ZU").",
                  thread->chunk->size_complete, thread->chunk->size);
              warn_msg(FN, "We trust libcurl and assume that's okay if single mode.");
              goto saldl_perform_success;
            }
            else {
              fatal(FN, "Returned CURLE_OK for chunk %"SAL_ZU", but completed size(%"SAL_ZU") != requested size(%"SAL_ZU").",
                  thread->chunk->idx ,thread->chunk->size_complete, thread->chunk->size);
            }
          }
        }
        else {
          return; // Avoid endless loop if server does not report file size
        }
        break;
      case CURLE_SSL_CONNECT_ERROR:
      case CURLE_SEND_ERROR: // 55: SSL_write() returned SYSCALL, errno = 32
        semi_fatal_retries++;
        retries++;
        if (semi_fatal_retries <= MAX_SEMI_FATAL_RETRIES) {
          warn_msg(FN, "libcurl returned semi-fatal (%d: %s) \
              while downloading chunk %"SAL_ZU", retry %d/%d, delay=%"SAL_ZU".",
              ret, thread->err_buf, thread->chunk->idx,
              semi_fatal_retries, MAX_SEMI_FATAL_RETRIES, delay);
          goto semi_fatal_perform_retry;
        } else {
          fatal(NULL, "libcurl returned semi-fatal (%d: %s) while downloading chunk %"SAL_ZU", max semi-fatal retries %u exceeded.", ret, thread->err_buf, thread->chunk->idx, MAX_SEMI_FATAL_RETRIES);
        }
      case CURLE_OPERATION_TIMEDOUT:
      case CURLE_PARTIAL_FILE: /* single mode */
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_CONNECT:
      case CURLE_GOT_NOTHING:
      case CURLE_RECV_ERROR:
      case CURLE_HTTP_RETURNED_ERROR:
        if (ret == CURLE_HTTP_RETURNED_ERROR) {
          curl_easy_getinfo(thread->ehandle, CURLINFO_RESPONSE_CODE, &response);
          if (response < 500) {
            fatal(NULL, "libcurl returned fatal error (%d: %s) while downloading chunk %"SAL_ZU".", ret, thread->err_buf, thread->chunk->idx);
          } else {
            info_msg(NULL, "libcurl returned (%d: %s) while downloading chunk %"SAL_ZU", restarting (retry %"SAL_ZU", delay=%"SAL_ZU").", ret, thread->err_buf, thread->chunk->idx, ++retries, delay);
          }
        } else {
          info_msg(NULL, "libcurl returned (%d: %s) while downloading chunk %"SAL_ZU", restarting (retry %"SAL_ZU", delay=%"SAL_ZU").", ret, thread->err_buf, thread->chunk->idx, ++retries, delay);
        }
semi_fatal_perform_retry:
        sleep(delay);
        thread->reset_storage(thread);
        delay=retries*3/2;
        break;
      default:
        fatal(NULL, "libcurl returned fatal error (%d: %s) while downloading chunk %"SAL_ZU".", ret, thread->err_buf, thread->chunk->idx);
        break;
    }
  }
saldl_perform_success: ;
}

void* thread_func(void* threadS) {
  /* Block signals first */
  saldl_block_sig_pth();

  /* Detach so we don't have to waste time joining it */
  pthread_detach(pthread_self());

  thread_s* tmp = threadS;
  set_chunk_progress(tmp->chunk, PRG_STARTED);
  saldl_perform(tmp);
  set_chunk_progress(tmp->chunk, PRG_FINISHED);
  return threadS;
}

void curl_cleanup(info_s *info_ptr) {

  for (size_t counter = 0; counter < info_ptr->params->num_connections; counter++) {
    curl_easy_cleanup(info_ptr->threads[counter].ehandle);
  }

  curl_global_cleanup();
}

/* vim: set filetype=c ts=2 sw=2 et spell foldmethod=syntax: */
