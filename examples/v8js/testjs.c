#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#include "libmill.h"
#include "jsv8.h"

void js_panic(js_vm *vm) {
    fprintf(stderr, "%s\n", js_errstr(vm));
    exit(1);
}
#define CHECK(rc, vm) if(!rc) js_panic(vm)


void testcall(js_vm *vm) {
    js_handle *list_props = js_eval(vm,
"(function (o){\n\
    let objToInspect;\n\
    let res = [];\n\
    for(objToInspect = o; objToInspect !== null;\n\
        objToInspect = Object.getPrototypeOf(objToInspect)){\n\
            res = res.concat(Object.getOwnPropertyNames(objToInspect));\n\
    }\n\
    res.sort();\n\
    for (let i = 0; i < res.length; i++)\n\
        $print(res[i]);\n\
    }\n\
)");

    CHECK(list_props, vm);
    assert(js_isfunction(list_props));
    js_handle *h1 = js_call(vm, list_props, NULL,
                        (jsargs_t) { JSGLOBAL(vm) });
    CHECK(h1, vm);
    js_dispose(h1);
    js_dispose(list_props);
}

coroutine void do_task1(js_vm *vm, js_coro *cr, js_handle *inh) {
    yield();
    const char *s1 = js_tostring(inh);
    fprintf(stderr, "<- %s\n", s1);
    int k = random() % 50;
    mill_sleep(now() + k);
    char tmp[100];
    sprintf(tmp, "%s -> Task done in %d millsecs ...", s1, k);
    js_handle *oh = js_string(vm, tmp, strlen(tmp));
    js_send(cr, oh, 0);  /* oh and inh disposed by V8 */
}

/* Coroutine in the V8 thread (concurrency) */
void testgo(js_vm *vm) {
    js_handle *p1 = js_pointer(vm, (void *) do_task1);
    /* p1 is a js object, can set properties on it */
    int r = js_set_string(p1, "name", "task1");
    assert(r);

    js_handle *f1 = js_callstr(vm, "(function(co) {\
        $print('co name =', co.name); \
        return function(s, callback) {\
            $go(co, s, callback);\
        };\
    });", NULL, (jsargs_t) { p1 } );
    assert(f1);

    /* Global.task1 = f1; */
    int rc = js_set(JSGLOBAL(vm), "task1", f1);
    assert(rc);
    js_dispose(f1);
    js_dispose(p1);

    rc = js_run(vm,
"for(var i=1; i<=5;i++) {\n\
    task1('go'+i, function (err, data) {\n\
            if (err == null) $print(data);\n\
    });\n\
}\n"
"$msleep(35);\n"
"for(var i=6; i<=10;i++) {\n\
    task1('go'+i, function (err, data) {\n\
        if (err == null) $print(data);\n\
    });\n\
}\n"
    );

    CHECK(rc, vm);
}

/* Coroutine in the main thread (concurrency & parallelism) */
void testsend(js_vm *vm) {
    js_handle *p1 = js_pointer(vm, (void *) do_task1);
    js_handle *f1 = js_callstr(vm, "(function(co) {\
        return function(s, callback) {\
            $send(co, s, callback);\
        };\
    });", NULL, (jsargs_t) { p1 } );
    assert(f1);

    /* Global.task1 = f1; */
    int rc = js_set(JSGLOBAL(vm), "task2", f1);
    assert(rc);
    js_dispose(f1);
    js_dispose(p1);

    rc = js_run(vm,
"for(var i=1; i<=5;i++) {\n\
    task2('send'+i, function (err, data) {\n\
            if (err == null) $print(data);\n\
    });\n\
}\n"
    );

    CHECK(rc, vm);
}


static char *readfile(const char *filename, size_t *len);

js_handle *ff_readfile(js_vm *vm, int argc, js_handle *argv[]) {
    const char *filename = js_tostring(argv[0]);
    size_t sz;
    char *buf = readfile(filename, & sz);
    js_handle *ret;
    if (buf) {
        ret = js_string(vm, buf, sz);
        free(buf);
    } else
        ret = JSNULL(vm);
    return ret;
}

js_handle *ff_strerror(js_vm *vm, int argc, js_handle *argv[]) {
    int errnum = (int) js_tonumber(argv[0]);
    char *s = strerror(errnum);
    js_handle *ret = js_string(vm, s, strlen(s));
    return ret;
}

static js_ffn_t ff_table[] = {
    { 1, ff_readfile, "readfile" },
    { 1, ff_strerror, "strerror" },
};

/* Create an object with the exported C functions */
js_handle *exports(js_vm *vm) {
    int i;
    int n = sizeof (ff_table) / sizeof (ff_table[0]);
    js_handle *h1 = js_object(vm);
    for (i = 0; i < n; i++) {
        js_handle *f1 = js_ffn(vm, &ff_table[i]);
        if (! js_set(h1, ff_table[i].name, f1))
            js_panic(vm);
        js_dispose(f1);
    }
    return h1;
}

void testexports(js_vm *vm) {
    js_handle *eh = exports(vm);
    js_set(JSGLOBAL(vm), "c", eh);
    js_dispose(eh);
    int rc = js_run(vm,
"var s = c.readfile('./testjs.c');\n\
if (s !== null) $print(s.substring(0, 20) + ' ...');"
    );
    CHECK(rc, vm);
}


int main(int argc, char *argv[]) {
    mill_init(-1, 0);
    mill_worker w = mill_worker_create();
    js_vm *vm = js_vmopen(w);

    testcall(vm);
    testgo(vm);
    testexports(vm);
    testsend(vm);
    js_vmclose(vm);
    mill_worker_delete(w);
    mill_fini();
    return 0;
}


static char *readfile(const char *filename, size_t *len) {
    int fd = open(filename, 0, 0666);
    if (fd < 0)
        goto er;
    struct stat sbuf;
    if (fstat(fd, & sbuf) != 0)
        goto er;
    if (S_ISDIR(sbuf.st_mode)) {
        errno = EISDIR;
        goto er;
    }
    size_t size = sbuf.st_size;
    char *buf = malloc(size + 1);
    if (!buf) {
        errno = ENOMEM;
        goto er;
    }
    if (read(fd, buf, size) != size) {
        free(buf);
        goto er;
    }
    close(fd);
    buf[size] = '\0';
    *len = size;
    return buf;
er:
    if (fd >= 0)
        close(fd);
    return NULL;
}

