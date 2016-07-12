#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

#define PP_ARRAY_COUNT(ARRAY) \
    (sizeof((ARRAY))/sizeof(*(ARRAY)))
#define PP_ARRAY_FOR(NAME, ARRAY) \
    for (size_t NAME = 0; NAME < PP_ARRAY_COUNT(ARRAY); NAME++)

static void lcc_usage(const char *app) {
    fprintf(stderr, "%s usage: [--lambda-pp=<path/to/lambda-pp>] <cc to use> [cc options]\n", app);
}

static void lcc_error(const char *message,  ...) {
    fprintf(stderr, "error: ");
    va_list va;
    va_start(va, message);
    vfprintf(stderr, message, va);
    fprintf(stderr, "\n");
    va_end(va);
}

typedef struct {
    const char *file;
    size_t      index;
    bool        cpp;
} lcc_source_t;

typedef struct {
    const char *output;
    size_t      index;
    bool        aout;
} lcc_output_t;

typedef struct {
    char   *buffer;
    size_t  used;
    size_t  allocated;
} lcc_string_t;

static bool lcc_string_init(lcc_string_t *string) {
    if (!(string->buffer = malloc(64)))
        return false;
    string->buffer[0] = '\0';
    string->used      = 1; /* always use the null byte */
    string->allocated = 64;
    return 1;
}

static bool lcc_string_resize(lcc_string_t *string) {
    size_t request = string->allocated * 2;
    void  *attempt = realloc(string->buffer, request);
    if (!attempt)
        return false;
    string->allocated = request;
    string->buffer    = attempt;
    return true;
}

static bool lcc_string_appendf(lcc_string_t *string, const char *message, ...) {
    va_list va1, va2;
    va_start(va1, message);
    va_copy(va2, va1); /* Copy the list */
    size_t count = vsnprintf(NULL, 0, message, va1);
    while (string->used + count >= string->allocated) {
        if (!lcc_string_resize(string))
            return false;
    }
    va_end(va1);

    va_start(va2, message);
    vsnprintf(string->buffer + string->used - 1, count + 1, message, va2);
    va_end(va2);
    string->used += count;
    return true;
}

static void lcc_string_destroy(lcc_string_t *string) {
    free(string->buffer);
}

static char *lcc_lambdapp_find(void) {
    char *search;
    if ((search = getenv("LAMBDA_PP")))
        return strdup(search);

    const char *PATHS = getenv("PATH");
    char *paths = strdup(PATHS ? PATHS : ".:/bin:/usr/bin:lambdapp");
    char *lambdapp = NULL;
    char *tok = NULL;

    while ((tok = strtok(tok ? NULL : paths, ":")) != NULL) {
        char path[PATH_MAX];
        struct stat st;

        snprintf(path, PATH_MAX - 1, "%s/lambda-pp", tok);
        if (stat(path, &st) != 0) continue;
        if ((st.st_mode & S_IXUSR) == 0) continue;
        lambdapp = strdup(path);
        break;
    }

    free(paths);
    return lambdapp;

#if 0
    PP_ARRAY_FOR(bin, bins) {
        DIR *dir = opendir(bins[bin]);
        if (!dir)
            continue;

        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_type != DT_REG && entry->d_type != DT_LNK)
                continue;

            if (!strcmp(entry->d_name, "lambda-pp")) {
                closedir(dir);
                return bins[bin];
            }
        }
        closedir(dir);
    }
    return NULL;
#endif
}

#if 0
static const char *lcc_compiler_find(void) {
    /* Try enviroment variables first */
    char *search;
    if ((search = getenv("CC")))
        return search;
    if ((search = getenv("CXX")))
        return search;

    /* Start searching toolchain directories */
    static const char *bins[] = {
        "/bin",
        "/usr/bin",
    };

    static const char *ccs[] = {
        "cc", "gcc", "clang", "pathcc", "tcc"
    };

    PP_ARRAY_FOR(bin, bins) {
        DIR *dir = opendir(bins[bin]);
        if (!dir)
            continue;

        PP_ARRAY_FOR(cc, ccs) {
            /* Scan the directory for one of the CCs */
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                /* Ignore things which are not files */
                if (entry->d_type != DT_REG && entry->d_type != DT_LNK)
                    continue;
                if (!strcmp(entry->d_name, ccs[cc])) {
                    closedir(dir);
                    return ccs[cc];
                }
            }
        }
        closedir(dir);
    }

    return NULL;
}
#endif

static bool lcc_source_find(int argc, char **argv, lcc_source_t *source) {
    static const char *exts[] = {
        /* C file extensions */
        ".c",
        ".C",

        /* C++ file extensions */
        ".cc", ".cx", ".cxx", ".cpp",
        ".CC", ".CX", ".CXX", ".CPP",
    };

    for (int i = 0; i < argc; i++) {
        PP_ARRAY_FOR(ext, exts) {
            char *find = strstr(argv[i], exts[ext]);
            if (!find)
                continue;

            /* It could be named stupidly like foo.c.c so we scan the whole filename
             * until we reach the end (when strcmp will succeed).
             */
            while (find) {
                /* Make sure it's the end of the string */
                if (!strcmp(find, exts[ext])) {
                    source->index = i;
                    source->file  = argv[i];
                    source->cpp   = (ext >= 1); /* See table of sources above for when this is valid. */
                    return true;
                }
                find = strstr(find + strlen(exts[ext]), exts[ext]);
            }
        }
    }
    return false;
}

static bool lcc_output_find(int argc, char **argv, lcc_output_t *output) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o"))
            continue;
        if (i + 1 >= argc) /* expecting output */
            return false;
        output->index  = i;
        output->output = argv[i + 1];
        return true;
    }
    return false;
}

static char * lcc_compiler_from_argv(int *pargc, char ***pargv) {
    // This is quite crappy! configure seems to break up "strings like this"
    // into multiple args when doing CC="lambda-pp 'ccache clang'"
    // Only supports ", no ', nor \' , nor \"

      char *cc = NULL;
    int argc = *pargc;
    char **argv = *pargv;

    if (!argc || !argv[0]) return NULL;
    if ((argv[0][0] == '"') || (argv[0][0] == '\'')) {
        const char delim = argv[0][0];
        int end = 0;
        int len = strlen(argv[0]);
        char *last = NULL;

        for (int i = 1; i < argc; i++) {
            len += strlen(argv[i]);
            if (strchr(argv[i], delim)) {
                end = i;
                last = strdup(argv[i]);
                break;
            }
        }
        if (!end) return NULL;

        cc = calloc(len + 1, 1);
        *(strchr(last, delim)) = '\0';
        strcpy(cc, argv[0] + 1);
        strcat(cc, " ");
        for (int i = 1; i < end; i++) {
            strcat(cc, argv[i]);
            strcat(cc, " ");
        }
        strcat(cc, last);

        free(last);
        *pargc -= end;
        *pargv += end;
        return cc;
    }

    return strdup(argv[0]);
}

static char *cmd_sanitize(const char *cmd) {
    char *sane;
    int cnt = 0, i, k;

    for (i = 0; cmd[i]; i++)
         if (cmd[i] == '"') cnt++;

    if (!cnt) return strdup(cmd);

    sane = malloc(i + cnt + 1);
    for (i = 0, k = 0; cmd[i]; i++) {
         if (cmd[i] == '"')
             sane[k++] = '\\';
         sane[k++] = cmd[i];
    }
    sane[k] = 0;
    return sane;
}

int main(int argc, char **argv) {
    int compile_only = 0;
    argc--;
    argv++;

    if (argc < 2) {
        lcc_usage(argv[-1]);
        return 1;
    }

    char *lambdapp = NULL;

    if (!strncmp("--lambda-pp", argv[0], sizeof("--lambda-pp") - 1)) {
        if (argv[0][sizeof("--lambda-pp")] != '=') {
            lambdapp = strdup(argv[1]);
            argv += 2;
            argc -= 2;
        } else {
            lambdapp = strdup(argv[0] + sizeof("--lambda-pp"));
            argv++;
            argc--;
        }
    } else {
        lambdapp = lcc_lambdapp_find();
    }

    if (argc < 1) {
        lcc_usage(argv[-1]);
        goto args_fail;
    }

    if (!lambdapp) {
        lcc_error("Couldn't find lambda-pp");
        goto args_fail;
    }

    char *cc = lcc_compiler_from_argv(&argc, &argv);
    if (!cc) {
        lcc_error("Couldn't find a compiler");
        goto args_fail;
    }

    argv++;
    argc--;
    if (argc < 1) {
        lcc_usage(argv[-1]);
        return 1;
    }

    /* Get the arguments */
    lcc_string_t args_before; /* before -o */
    lcc_string_t args_after;  /* after -o */
    if (!lcc_string_init(&args_before)) {
        lcc_error("Out of memory");
        return 1;
    }
    if (!lcc_string_init(&args_after)) {
        lcc_error("Out of memory");
        lcc_string_destroy(&args_before);
        return 1;
    }

    /* Find the source file */
    lcc_source_t source;
    if (!lcc_source_find(argc, argv, &source)) {
        /* If there isn't any source file on the command line it means
         * the compiler is being used to invoke the linker.
         */
        lcc_string_destroy(&args_after);
        lcc_string_appendf(&args_before, "%s", cc);
        for (int i = 0; i < argc; i++) {
            if (!lcc_string_appendf(&args_before, " %s", argv[i]))
                goto args_oom;
        }

        int attempt = system(args_before.buffer);
        lcc_string_destroy(&args_before);
        return attempt;
    }

    /* Find the output file */
    lcc_output_t output = { .aout = false };
    if (!lcc_output_find(argc, argv, &output)) {
        /* not found? default to a.out */
        output.output = "a.out";
        output.aout = true;
    }
    
    /* Stop at the -o */
    size_t stop = output.aout ? (size_t)argc : output.index;
    for (size_t i = 0; i < stop; i++) {
        /* Ignore the source file */
        if (i == source.index)
            continue;
        if (!lcc_string_appendf(&args_before, "%s ", argv[i]))
            goto args_oom;
        if (!strcmp(argv[i], "-c")) compile_only = 1;
    }
    /* Trim the trailing whitespace */
    if (args_before.used >= 2)
        args_before.buffer[args_before.used - 2] = '\0';
   
    /* Handle anythng after the -o */
//    stop += 2; /* skip -o and <output> */
    size_t count = argc;
    for (size_t i = stop; i < count; i++) {
        if (i == source.index) continue;
        if (!lcc_string_appendf(&args_after, "%s ", argv[i]))
            goto args_oom;
        if (!strcmp(argv[i], "-c")) compile_only = 1;
    }

    /* Build the shell call */
    lcc_string_t shell;
    if (!lcc_string_init(&shell))
        goto args_oom;

    char *srcdir = strdup(source.file);
    char *dirsep = strrchr(srcdir, '/');
    if (dirsep) {
        *dirsep = 0;
        lcc_string_appendf(&args_after, "-I%s ", srcdir);
    }
    free(srcdir);

    /* Trim trailing whitespace */
    if (args_after.used >= 2)
        args_after.buffer[args_after.used - 2] = '\0';

    const char *lang = source.cpp ? "c++" : "c";
#if 0
    if (!lcc_string_appendf(&shell, "%s %s | %s -x%s %s - -o %s %s",
        lambdapp, source.file, cc, lang, args_before.buffer, output.output, args_after.buffer))
            goto shell_oom;
#else
    // FIXME: .o extension depends on the platform
    if (compile_only && output.aout) {
        if (!lcc_string_appendf(&shell, "%s %s | %s -x%s %s %s -o %s.o -",
            lambdapp, source.file, cc, lang, args_before.buffer, args_after.buffer, source.file))
            goto shell_oom;
    } else if (!lcc_string_appendf(&shell, "%s %s | %s -x%s %s %s -",
        lambdapp, source.file, cc, lang, args_before.buffer, args_after.buffer))
            goto shell_oom;
#endif

    char *sanecmd = cmd_sanitize(shell.buffer);

    int attempt = 0;
#ifndef _NDEBUG
    /* Call the shell */
    attempt = system(sanecmd);
#else
    printf("%s\n", sanecmd);
#endif

    lcc_string_destroy(&shell);
    lcc_string_destroy(&args_before);
    lcc_string_destroy(&args_after);
    free(lambdapp);
    free(sanecmd);

    return attempt;

shell_oom:
    lcc_string_destroy(&shell);
args_oom:
    lcc_error("Out of memory");
    lcc_string_destroy(&args_before);
    lcc_string_destroy(&args_after);
args_fail:
    free(lambdapp);
    return 1;
}
