/* See texcaller.h for copyright information and documentation. */

/*! \addtogroup libtexcaller
 *  @{
 *
 *  \defgroup libtexcaller_internals Internals of the Texcaller library
 *  @{
 */

#define _BSD_SOURCE

#include "texcaller.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*! Escape a single character for LaTeX.
 *
 *  \param c
 *      the character to escape
 *
 *  \return
 *      a string constant (not to be freed)
 *      containing the character's replacement,
 *      or \c NULL if the character doesn't need to be replaced.
 */
static const char *escape_latex_char(char c)
{
    switch (c) {
        case '$':  return "\\$";
        case '%':  return "\\%";
        case '&':  return "\\&";
        case '#':  return "\\#";
        case '_':  return "\\_";
        case '{':  return "\\{";
        case '}':  return "\\}";
        case '[':  return "{[}";
        case ']':  return "{]}";
        case '"':  return "{''}";
        case '\\': return "\\textbackslash{}";
        case '~':  return "\\textasciitilde{}";
        case '<':  return "\\textless{}";
        case '>':  return "\\textgreater{}";
        case '^':  return "\\textasciicircum{}";
        case '`':  return "{}`"; /* avoid ?` and !` */
        case '\n': return "\\\\";
        default:   return NULL;
    }
}

/*! Variant of \c sprintf() that allocates the needed memory automatically.
 *
 *  \param format
 *      format string for sprintf()
 *
 *  \param ...
 *      further arguments to sprintf()
 *
 *  \return
 *      a newly allocated string containing the result of \c sprintf(),
 *      or \c NULL when out of memory or sprintf() failed.
 */
static char *sprintf_alloc(const char *format, ...)
{
    va_list ap;
    char tmp_result[1];
    char *result;
    int len;
    int len_written;
    /* calculate result size */
    va_start(ap, format);
    len = vsnprintf(tmp_result, sizeof(tmp_result), format, ap);
    va_end(ap);
    if (len <= 0) {
        return NULL;
    }
    /* allocate memory for result */
    result = malloc(len + 1);
    if (result == NULL) {
        return NULL;
    }
    /* calculate result */
    va_start(ap, format);
    len_written = vsnprintf(result, len + 1, format, ap);
    va_end(ap);
    if (len_written != len) {
        free(result);
        return NULL;
    }
    return result;
}

/*! Remove a directory recursively like <tt>rm -r</tt>.
 *
 *  \return
 *      0 on success, -1 on failure
 *
 *  \param error
 *      On failure, \c error will be set to a newly allocated string
 *      that contains the error message.
 *      On success, or when out of memory,
 *      \c error will be set to \c NULL.
 *
 *  \param dirname
 *      the directory to remove
 */
static int remove_directory_recursively(char **error, const char *dirname)
{
    DIR *dir;
    struct dirent *entry;
    /* continue on errors and report only the first error that occured */
    *error = NULL;
    dir = opendir(dirname);
    if (dir == NULL && *error == NULL) {
        *error = sprintf_alloc("Unable to read directory entries of \"%s\": %s.",
                               dirname, strerror(errno));
    } else {
        for (entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
            char *name;
            if (   strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            name = sprintf_alloc("%s/%s", dirname, entry->d_name);
            if (name == NULL && *error == NULL) {
                *error = sprintf_alloc("Out of memory.");
                continue;
            }
            if (entry->d_type == DT_DIR) {
                char *sub_error;
                if (remove_directory_recursively(&sub_error, name) != 0 && *error == NULL) {
                    *error = sub_error;
                }
            } else {
                if (unlink(name) != 0 && *error == NULL) {
                    *error = sprintf_alloc("Unable to remove file \"%s\": %s.",
                                           name, strerror(errno));
                }
            }
            free(name);
        }
        if (closedir(dir) != 0 && *error == NULL) {
            *error = sprintf_alloc("Unable to close directory \"%s\": %s.",
                                   dirname, strerror(errno));
        }
    }
    if (rmdir(dirname) != 0) {
        if (*error == NULL) {
            *error = sprintf_alloc("Unable to remove directory \"%s\": %s.",
                                   dirname, strerror(errno));
        }
        return -1;
    }
    /* all previous errors are irrelevant because rmdir() was successful */
    *error = NULL;
    return 0;
}

/*! Read a file completely into a buffer that can be used as a string.
 *
 *  \param dest
 *      will be set to a newly allocated buffer that contains
 *      the complete content of the file,
 *      with a \c '\\0' added to the end.
 *      If an error occured,
 *      \c dest will be set to \c NULL.
 *
 *  \param dest_size
 *      will be set to the size of \c dest,
 *      not counting the added \c '\\0'.
 *      If an error occured,
 *      \c dest_size will be set to \c 0.
 *
 *  \param error
 *      On failure, \c error will be set to a newly allocated string
 *      that contains the error message.
 *      On success, or when out of memory,
 *      \c error will be set to \c NULL.
 *
 *  \param path
 *      path of the file to read
 */
static void read_file(char **dest, size_t *dest_size, char **error, const char *path)
{
    FILE *file;
    long file_size;
    size_t read_size;
    *dest = NULL;
    *error = NULL;
    file = fopen(path, "rb");
    if (file == NULL) {
        *error = sprintf_alloc("Unable to open file \"%s\" for reading: %s.",
                               path, strerror(errno));
        goto error_cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        *error = sprintf_alloc("Unable to seek to end of file \"%s\": %s.",
                               path, strerror(errno));
        goto error_cleanup;
    }
    file_size = ftell(file);
    if (file_size == -1) {
        *error = sprintf_alloc("Unable to obtain size of file \"%s\": %s.",
                               path, strerror(errno));
        goto error_cleanup;
    }
    *dest_size = file_size;
    if (fseek(file, 0, SEEK_SET) != 0) {
        *error = sprintf_alloc("Unable to seek back to start of file \"%s\": %s.",
                               path, strerror(errno));
        goto error_cleanup;
    }
    *dest = malloc(*dest_size + 1);
    if (*dest == NULL) {
        *error = sprintf_alloc("Unable to allocate buffer for reading file \"%s\": %s.",
                               path, strerror(errno));
        goto error_cleanup;
    }
    (*dest)[*dest_size] = '\0';
    read_size = fread(*dest, 1, *dest_size, file);
    if (ferror(file)) {
        *error = sprintf_alloc("Unable to read %lu bytes from file \"%s\": %s.",
                               (unsigned long)*dest_size, path, strerror(errno));
        goto error_cleanup;
    }
    if (read_size != *dest_size) {
        *error = sprintf_alloc("Unable to read %lu bytes from file \"%s\": Got only %lu bytes.",
                               (unsigned long)*dest_size, path, (unsigned long)read_size);
        goto error_cleanup;
    }
    if (fclose(file) != 0) {
        *error = sprintf_alloc("Unable to close file \"%s\" after reading: %s.",
                               path, strerror(errno));
        file = NULL;
        goto error_cleanup;
    }
    return;
error_cleanup:
    free(*dest);
    *dest = NULL;
    *dest_size = 0;
    if (file != NULL) {
        fclose(file);
    }
}

/*! Write a buffer completely into a file.
 *
 *  If the file already exists, it will be overwritten.
 *
 *  \return
 *      0 on success, -1 on failure
 *
 *  \param error
 *      On failure, \c error will be set to a newly allocated string
 *      that contains the error message.
 *      On success, or when out of memory,
 *      \c error will be set to \c NULL.
 *
 *  \param path
 *      path of the file to write to
 *
 *  \param src
 *      buffer to write
 *
 *  \param src_size
 *      size of \c src
 */
static int write_file(char **error, const char *path, const char *src, size_t src_size)
{
    FILE *file;
    size_t written_size;
    *error = NULL;
    file = fopen(path, "wb");
    if (file == NULL) {
        *error = sprintf_alloc("Unable to open file \"%s\" for writing: %s.",
                               path, strerror(errno));
        goto error_cleanup;
    }
    written_size = fwrite(src, 1, src_size, file);
    if (ferror(file)) {
        *error = sprintf_alloc("Unable to write %lu bytes to file \"%s\": %s.",
                               (unsigned long)src_size, path, strerror(errno));
        goto error_cleanup;
    }
    if (written_size != src_size) {
        *error = sprintf_alloc("Unable to write %lu bytes to file \"%s\": Only %lu bytes were written.",
                               (unsigned long)src_size, path, (unsigned long)written_size);
        goto error_cleanup;
    }
    if (fclose(file) != 0) {
        *error = sprintf_alloc("Unable to close file \"%s\" after writing: %s.",
                               path, strerror(errno));
        file = NULL;
        goto error_cleanup;
    }
    return 0;
error_cleanup:
    if (file != NULL) {
        fclose(file);
    }
    return -1;
}

/*!  @} */

/*! Convert a TeX or LaTeX source to DVI or PDF.
 */
void texcaller_convert(char **dest, size_t *dest_size, char **info, const char *src, size_t src_size, const char *src_format, const char *dest_format, int max_runs)
{
    char *error;
    char *cmd;
    char *tmpdir;
    char *dir = NULL;
    char *dir_template = NULL;
    char *src_filename = NULL;
    char *aux_filename = NULL;
    char *log_filename = NULL;
    char *dest_filename = NULL;
    char *aux = NULL;
    size_t aux_size = 0;
    char *aux_old = NULL;
    size_t aux_old_size = 0;
    int runs;
    *dest = NULL;
    *dest_size = 0;
    *info = NULL;
    /* check arguments */
    if        (strcmp(src_format, "TeX") == 0 && strcmp(dest_format, "DVI") == 0) {
        cmd = "tex";
    } else if (strcmp(src_format, "TeX") == 0 && strcmp(dest_format, "PDF") == 0) {
        cmd = "pdftex";
    } else if (strcmp(src_format, "LaTeX") == 0 && strcmp(dest_format, "DVI") == 0) {
        cmd = "latex";
    } else if (strcmp(src_format, "LaTeX") == 0 && strcmp(dest_format, "PDF") == 0) {
        cmd = "pdflatex";
    } else {
        *info = sprintf_alloc("Unable to convert from \"%s\" to \"%s\".",
                              src_format, dest_format);
        goto cleanup;
    }
    if (max_runs < 2) {
        *info = sprintf_alloc("Argument max_runs is %i, but must be >= 2.",
                              max_runs);
        goto cleanup;
    }
    /* create temporary directory */
    tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || strcmp(tmpdir, "") == 0) {
        tmpdir = "/tmp";
    }
    dir_template = sprintf_alloc("%s/texcaller-temp-XXXXXX", tmpdir);
    if (dir_template == NULL) {
        goto cleanup;
    }
    dir = mkdtemp(dir_template);
    if (dir == NULL) {
        *info = sprintf_alloc("Unable to create temporary directory from template \"%s\": %s.",
                              dir_template, strerror(errno));
        goto cleanup;
    }
    src_filename = sprintf_alloc("%s/texput.tex", dir);
    if (src_filename == NULL) {
        goto cleanup;
    }
    aux_filename = sprintf_alloc("%s/texput.aux", dir);
    if (aux_filename == NULL) {
        goto cleanup;
    }
    log_filename = sprintf_alloc("%s/texput.log", dir);
    if (log_filename == NULL) {
        goto cleanup;
    }
    if (strcmp(dest_format, "DVI") == 0) {
        dest_filename = sprintf_alloc("%s/texput.dvi", dir);
    } else {
        dest_filename = sprintf_alloc("%s/texput.pdf", dir);
    }
    if (dest_filename == NULL) {
        goto cleanup;
    }
    /* create source file */
    if (write_file(&error, src_filename, src, src_size) != 0) {
        *info = error;
        goto cleanup;
    }
    /* run command as often as necessary */
    for (runs = 1; runs <= max_runs; runs++) {
        pid_t pid;
        pid = fork();
        if (pid == -1) {
            *info = sprintf_alloc("Unable to fork child process: %s.",
                                  strerror(errno));
            goto cleanup;
        }
        /* child process */
        if (pid == 0) {
            /* run command within the temporary directory */
            if (chdir(dir) != 0) {
                exit(1);
            }
            /* prevent access to stdin, stdout and stderr */
            fclose(stdin);
            fclose(stdout);
            fclose(stderr);
            /* execute command */
            execlp(cmd,
                   cmd,
                   "-interaction=batchmode",
                   "-halt-on-error",
                   "-no-shell-escape",
                   "-file-line-error",
                   "texput.tex",
                   NULL);
        }
        /* wait for child process */
        for (;;) {
            int status;
            pid_t wpid = waitpid(pid, &status, 0);
            if (wpid == -1) {
                *info = sprintf_alloc("Unable to wait for child process: %s.",
                                      strerror(errno));
                goto cleanup;
            }
            if (WIFSIGNALED(status)) {
                *info = sprintf_alloc("Command \"%s\" was terminated by signal %i.",
                                      cmd, (int)WTERMSIG(status));
                goto cleanup;
            }
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                *info = sprintf_alloc("Command \"%s\" terminated with exit status %i.",
                                      cmd, (int)WEXITSTATUS(status));
                goto cleanup;
            }
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                break;
            }
        }
        /* read new aux file, saving old one */
        free(aux_old);
        aux_old      = aux;
        aux_old_size = aux_size;
        read_file(&aux, &aux_size, &error, aux_filename);
        /* tolerate missing aux file */
        free(error);
        /* check whether aux file stabilized,
           which is also true if there isn't and wasn't any aux file */
        if (aux_size == aux_old_size && memcmp(aux, aux_old, aux_size) == 0) {
            read_file(dest, dest_size, &error, dest_filename);
            if (*dest == NULL) {
                *info = error;
                goto cleanup;
            }
            *info = sprintf_alloc("Generated %s (%lu bytes)"
                                  " from %s (%lu bytes) after %i runs.",
                                  dest_format, (unsigned long)*dest_size,
                                  src_format, (unsigned long)src_size, runs);
            goto cleanup;
        }
    }
    /* aux file didn't stabilize */
    *info = sprintf_alloc("Output didn't stabilize after %i runs.",
                          max_runs);
    goto cleanup;
    /* cleanup all used resources */
cleanup:
    if (log_filename != NULL) {
        char *log;
        size_t log_size;
        read_file(&log, &log_size, &error, log_filename);
        free(error);
        if (log != NULL) {
            if (*info == NULL) {
                *info = log;
            } else {
                char *info_old = *info;
                *info = sprintf_alloc("%s\n\n%s", info_old, log);
                free(info_old);
                free(log);
            }
        }
    }
    if (dir != NULL && remove_directory_recursively(&error, dir) != 0) {
        free(*dest);
        *dest = NULL;
        *dest_size = 0;
        free(*info);
        *info = error;
    }
    free(dir_template);
    free(src_filename);
    free(aux_filename);
    free(log_filename);
    free(dest_filename);
    free(aux);
    free(aux_old);
}

/*! Escape a string for direct use in LaTeX.
 */
char *texcaller_escape_latex(const char *s)
{
    char *escaped_string;
    size_t i;
    size_t length;
    size_t pos;
    /* calculate result length */
    length = 0;
    for (i = 0; s[i] != '\0'; i++) {
        const char *escaped_char = escape_latex_char(s[i]);
        if (escaped_char == NULL) {
            length++;
        } else {
            length += strlen(escaped_char);
        }
    }
    /* allocate memory for result */
    escaped_string = malloc(length + 1);
    if (escaped_string == NULL) {
        return NULL;
    }
    /* calculate result */
    pos = 0;
    for (i = 0; s[i] != '\0'; i++) {
        const char *escaped_char = escape_latex_char(s[i]);
        if (escaped_char == NULL) {
            escaped_string[pos++] = s[i];
        } else {
            const size_t length = strlen(escaped_char);
            memcpy(escaped_string + pos, escaped_char, length);
            pos += length;
        }
    }
    escaped_string[pos] = '\0';
    return escaped_string;
}

/*!  @} */
