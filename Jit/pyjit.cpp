// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include "Jit/pyjit.h"

#include "Python.h"
//#include "internal/pycore_pystate.h"
#include "Include/internal/pycore_pystate.h"
#include "internal/pycore_shadow_frame.h"

#include "Jit/code_allocator.h"
#include "Jit/codegen/gen_asm.h"
#include "Jit/containers.h"
#include "Jit/frame.h"
#include "Jit/hir/builder.h"
#include "Jit/hir/preload.h"
#include "Jit/inline_cache.h"
#include "Jit/jit_context.h"
#include "Jit/jit_flag_processor.h"
#include "Jit/jit_gdb_support.h"
#include "Jit/jit_list.h"
#include "Jit/jit_time_log.h"
#include "Jit/lir/inliner.h"
#include "Jit/log.h"
#include "Jit/perf_jitdump.h"
#include "Jit/profile_data.h"
#include "Jit/ref.h"
#include "Jit/runtime.h"
#include "Jit/type_profiler.h"
#include "Jit/util.h"

#include <dis-asm.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>

#define DEFAULT_CODE_SIZE 2 * 1024 * 1024

using namespace jit;

int64_t __strobe_CodeRuntime_py_code = CodeRuntime::kPyCodeOffset;

struct JitConfig {
  InitStateJitConfig init_state{JIT_NOT_INITIALIZED};

  int is_enabled{0};
  FrameModeJitConfig frame_mode{PY_FRAME};
  int are_type_slots_enabled{1};
  int allow_jit_list_wildcards{0};
  int compile_all_static_functions{0};
  size_t batch_compile_workers{0};
  int multithreaded_compile_test{0};
  bool use_huge_pages{true};
  bool multiple_code_sections{false};
  size_t hot_code_section_size{0};
  size_t cold_code_section_size{0};
  int hir_inliner_enabled{0};
};
static JitConfig jit_config;

void initJitConfig_() {
  jit_config = JitConfig();
}

int _PyJIT_IsJitConfigAllow_jit_list_wildcards() {
  return jit_config.allow_jit_list_wildcards;
}

int _PyJIT_IsJitConfigCompile_all_static_functions() {
  return jit_config.compile_all_static_functions;
}

size_t _PyJIT_GetJitConfigBatch_compile_workers() {
  return jit_config.batch_compile_workers;
}

int _PyJIT_IsJitConfigMultithreaded_compile_test() {
  return jit_config.multithreaded_compile_test;
}

namespace {
// Extra information needed to compile a PyCodeObject.
struct CodeData {
  CodeData(PyObject* m, PyObject* g) : module{m}, globals{g} {}

  Ref<> module;
  Ref<PyDictObject> globals;
};

// Amount of time taken to batch compile everything when disable_jit is called
long g_batch_compilation_time_ms = 0;

} // namespace

static _PyJITContext* jit_ctx;
static JITList* g_jit_list{nullptr};

// Function and code objects ("units") registered for compilation.
static std::unordered_set<BorrowedRef<>> jit_reg_units;
// Every unit that is a code object has corresponding entry in jit_code_data.
static std::unordered_map<BorrowedRef<PyCodeObject>, CodeData> jit_code_data;
// Every unit has an entry in preloaders if we are doing multithreaded compile.
static std::unordered_map<BorrowedRef<>, hir::Preloader> jit_preloaders;

namespace jit {
bool isPreloaded(BorrowedRef<PyFunctionObject> func) {
  return jit_preloaders.find(func) != jit_preloaders.end();
}

const jit::hir::Preloader& getPreloader(BorrowedRef<PyFunctionObject> func) {
  auto it = jit_preloaders.find(func);
  if (it != jit_preloaders.end()) {
    return it->second;
  }
  return map_get_strict(jit_preloaders, func->func_code);
}
} // namespace jit

static std::unordered_map<PyFunctionObject*, std::chrono::duration<double>>
    jit_time_functions;

// If non-empty, profile information will be written to this filename at
// shutdown.
static std::string g_write_profile_file;

// Frequently-used strings that we intern at JIT startup and hold references to.
#define INTERNED_STRINGS(X) \
  X(bc_offset)              \
  X(code_hash)              \
  X(count)                  \
  X(description)            \
  X(filename)               \
  X(firstlineno)            \
  X(func_qualname)          \
  X(guilty_type)            \
  X(int)                    \
  X(lineno)                 \
  X(normal)                 \
  X(normvector)             \
  X(opname)                 \
  X(reason)                 \
  X(types)

#define DECLARE_STR(s) static PyObject* s_str_##s{nullptr};
INTERNED_STRINGS(DECLARE_STR)
#undef DECLARE_STR

static std::array<PyObject*, 256> s_opnames;

static double total_compliation_time = 0.0;

int g_profile_new_interp_threads = 0;

struct CompilationTimer {
  explicit CompilationTimer(BorrowedRef<PyFunctionObject> f)
      : start(std::chrono::steady_clock::now()), func(f) {}

  ~CompilationTimer() {
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> time_span =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - start);

    double time = time_span.count();
    total_compliation_time += time;
    jit::ThreadedCompileSerialize guard;
    jit_time_functions.emplace(func, time_span);
  }

  std::chrono::steady_clock::time_point start;
  BorrowedRef<PyFunctionObject> func{nullptr};
};

static std::atomic<int> g_compile_workers_attempted;
static int g_compile_workers_retries;

void setJitLogFile(std::string log_filename) {
  // Redirect logging to a file if configured.
  const char* kPidMarker = "{pid}";
  std::string pid_filename = log_filename;
  auto marker_pos = pid_filename.find(kPidMarker);
  if (marker_pos != std::string::npos) {
    pid_filename.replace(
        marker_pos, std::strlen(kPidMarker), fmt::format("{}", getpid()));
  }
  FILE* file = fopen(pid_filename.c_str(), "w");
  if (file == NULL) {
    JIT_LOG(
        "Couldn't open log file %s (%s), logging to stderr",
        pid_filename,
        strerror(errno));
  } else {
    g_log_file = file;
  }
}

void setASMSyntax(std::string asm_syntax) {
  if (asm_syntax.compare("intel") == 0) {
    set_intel_syntax();
  } else if (asm_syntax.compare("att") == 0) {
    set_att_syntax();
  } else {
    JIT_CHECK(false, "unknown asm syntax '%s'", asm_syntax);
  }
}

static jit::FlagProcessor xarg_flag_processor;

static int use_jit = 0;
static int jit_help = 0;
static std::string write_profile_file;
static int jit_profile_interp = 0;
static std::string jl_fn;

void initFlagProcessor() {
  use_jit = 0;
  write_profile_file = "";
  jit_profile_interp = 0;
  jl_fn = "";
  jit_help = 0;
  if (!xarg_flag_processor.hasOptions()) {
    // flags are inspected in order of definition below
    xarg_flag_processor.addOption(
        "jit", "PYTHONJIT", use_jit, "Enable the JIT");

    xarg_flag_processor.addOption(
        "jit-debug",
        "PYTHONJITDEBUG",
        [](std::string) {
          g_debug = 1;
          g_debug_verbose = 1;
        },
        "JIT debug and extra logging");

    xarg_flag_processor
        .addOption(
            "jit-log-file",
            "PYTHONJITLOGFILE",
            [](std::string log_filename) { setJitLogFile(log_filename); },
            "write log entries to <filename> rather than stderr")
        .withFlagParamName("filename");

    xarg_flag_processor
        .addOption(
            "jit-asm-syntax",
            "PYTHONJITASMSYNTAX",
            [](std::string asm_syntax) { setASMSyntax(asm_syntax); },
            "set the assembly syntax used in log files")
        .withFlagParamName("intel|att")
        .withDebugMessageOverride("Sets the assembly syntax used in log files");

    xarg_flag_processor
        .addOption(
            "jit-debug-refcount",
            "PYTHONJITDEBUGREFCOUNT",
            g_debug_refcount,
            "JIT refcount insertion debug mode")
        .withDebugMessageOverride("Enabling");

    xarg_flag_processor
        .addOption(
            "jit-dump-hir",
            "PYTHONJITDUMPHIR",
            g_dump_hir,
            "log the HIR representation of all functions after initial "
            "lowering from bytecode")
        .withDebugMessageOverride("Dump initial HIR of JITted functions");

    xarg_flag_processor
        .addOption(
            "jit-dump-hir-line-numbers",
            "PYTHONJITDUMPHIRLINENUMBERS",
            g_dump_hir_line_numbers,
            "TODO")
        .withDebugMessageOverride("TODO");

    xarg_flag_processor
        .addOption(
            "jit-dump-hir-passes",
            "PYTHONJITDUMPHIRPASSES",
            g_dump_hir_passes,
            "log the HIR after each optimization pass")
        .withDebugMessageOverride(
            "Dump HIR of JITted functions after each individual  optimization "
            "pass");

    xarg_flag_processor
        .addOption(
            "jit-dump-final-hir",
            "PYTHONJITDUMPFINALHIR",
            g_dump_final_hir,
            "log the HIR after all optimizations")
        .withDebugMessageOverride(
            "Dump final HIR of JITted functions after all optimizations");

    xarg_flag_processor
        .addOption(
            "jit-dump-lir",
            "PYTHONJITDUMPLIR",
            g_dump_lir,
            "log the LIR representation of all functions after lowering from "
            "HIR")
        .withDebugMessageOverride("Dump initial LIR of JITted functions");

    xarg_flag_processor.addOption(
        "jit-dump-lir-no-origin",
        "PYTHONJITDUMPLIRNOORIGIN",
        [](std::string) {
          g_dump_lir = 1;
          g_dump_lir_no_origin = 1;
        },
        "JIT dump-lir mode without origin data");

    xarg_flag_processor.addOption(
        "jit-dump-c-helper",
        "PYTHONJITDUMPCHELPER",
        g_dump_c_helper,
        "dump all c invocations");

    xarg_flag_processor.addOption(
        "jit-disas-funcs",
        "PYTHONJITDISASFUNCS",
        g_dump_asm,
        "jit-disas-funcs/PYTHONJITDISASFUNCS are deprecated and will soon be "
        "removed. Use jit-dump-asm and PYTHONJITDUMPASM instead");

    xarg_flag_processor
        .addOption(
            "jit-dump-asm",
            "PYTHONJITDUMPASM",
            g_dump_asm,
            "log the final compiled code, annotated with HIR instructions")
        .withDebugMessageOverride("Dump asm of JITted functions");

    xarg_flag_processor.addOption(
        "jit-gdb-support",
        "PYTHONJITGDBSUPPORT",
        [](std::string) {
          g_debug = 1;
          g_gdb_support = 1;
        },
        "GDB support and JIT debug mode");

    xarg_flag_processor.addOption(
        "jit-gdb-stubs-support",
        "PYTHONJITGDBSTUBSSUPPORT",
        g_gdb_stubs_support,
        "GDB support for stubs");

    xarg_flag_processor.addOption(
        "jit-gdb-write-elf",
        "PYTHONJITGDBWRITEELF",
        [](std::string) {
          g_debug = 1;
          g_gdb_support = 1;
          g_gdb_write_elf_objects = 1;
        },
        "Debugging aid, GDB support with ELF output");

    xarg_flag_processor.addOption(
        "jit-dump-stats",
        "PYTHONJITDUMPSTATS",
        g_dump_stats,
        "Dump JIT runtime stats at shutdown");

    xarg_flag_processor.addOption(
        "jit-disable-lir-inliner",
        "PYTHONJITDISABLELIRINLINER",
        g_disable_lir_inliner,
        "disable JIT lir inlining");

    xarg_flag_processor.addOption(
        "jit-disable-huge-pages",
        "PYTHONJITDISABLEHUGEPAGES",
        [](std::string) { jit_config.use_huge_pages = false; },
        "disable huge page support");

    xarg_flag_processor.addOption(
        "jit-enable-jit-list-wildcards",
        "PYTHONJITENABLEJITLISTWILDCARDS",
        jit_config.allow_jit_list_wildcards,
        "allow wildcards in JIT list");

    xarg_flag_processor.addOption(
        "jit-all-static-functions",
        "PYTHONJITALLSTATICFUNCTIONS",
        jit_config.compile_all_static_functions,
        "JIT-compile all static functions");

    xarg_flag_processor
        .addOption(
            "jit-list-file",
            "PYTHONJITLISTFILE",
            [](std::string listFile) {
              jl_fn = listFile;
              use_jit = 1;
            },
            "Load list of functions to compile from <filename>")
        .withFlagParamName("filename");

    xarg_flag_processor
        .addOption(
            "jit-read-profile",
            "PYTHONJITREADPROFILE",
            [](std::string read_profile_file) {
              JIT_LOG("Loading profile data from %s", read_profile_file);
              readProfileData(read_profile_file);
            },
            "Load profile data from <filename>")
        .withFlagParamName("filename");

    xarg_flag_processor
        .addOption(
            "jit-write-profile",
            "PYTHONJITWRITEPROFILE",
            write_profile_file,
            "Write profiling data to <filename>")
        .withFlagParamName("filename");

    xarg_flag_processor.addOption(
        "jit-profile-interp",
        "PYTHONJITPROFILEINTERP",
        jit_profile_interp,
        "interpreter profiling");

    xarg_flag_processor.addOption(
        "jit-disable",
        "PYTHONJITDISABLE",
        [](int val) { use_jit = !val; },
        "disable the JIT");

    // these are only set if use_jit == 1
    xarg_flag_processor.addOption(
        "jit-shadow-frame",
        "PYTHONJITSHADOWFRAME",
        [](int val) {
          if (use_jit) {
            jit_config.frame_mode = val ? SHADOW_FRAME : PY_FRAME;
          }
        },
        "enable shadow frame mode");

    xarg_flag_processor.addOption(
        "jit-no-type-slots",
        "PYTHONJITNOTYPESLOTS",
        [](int val) {
          if (use_jit) {
            jit_config.are_type_slots_enabled = !val;
          }
        },
        "turn off type slots");

    xarg_flag_processor
        .addOption(
            "jit-batch-compile-workers",
            "PYTHONJITBATCHCOMPILEWORKERS",
            jit_config.batch_compile_workers,
            "set the number of batch compile workers to <COUNT>")
        .withFlagParamName("COUNT");

    xarg_flag_processor
        .addOption(
            "jit-multithreaded-compile-test",
            "PYTHONJITMULTITHREADEDCOMPILETEST",
            [](int val) {
              if (use_jit) {
                jit_config.multithreaded_compile_test = val;
              }
            },
            "JIT multithreaded compile test")
        .isHiddenFlag(true);

    xarg_flag_processor.addOption(
        "jit-list-match-line-numbers",
        "PYTHONJITLISTMATCHLINENUMBERS",
        [](int val) {
          if (use_jit) {
            jitlist_match_line_numbers(val);
          }
        },
        "JIT list match line numbers");

    xarg_flag_processor
        .addOption(
            "jit-time",
            "",
            [](std::string flag_value) { parseAndSetFuncList(flag_value); },
            "Measure time taken in compilation phases and output summary to "
            "stderr or approperiate logfile. Only functions in comma seperated "
            "<function_list> list will be included. Comma seperated list may "
            "include wildcards, * and ?. Wildcards are processed in glob "
            "fashion and not as regex.")
        .withFlagParamName("function_list")
        .withDebugMessageOverride(
            "Will capture time taken in compilation phases and output summary");
    ;

    xarg_flag_processor.addOption(
        "jit-enable-hir-inliner",
        "PYTHONJITENABLEHIRINLINER",
        [](int val) {
          if (use_jit && val) {
            _PyJIT_EnableHIRInliner();
          }
        },
        "Enable the JIT's HIR inliner");

    xarg_flag_processor.addOption(
        "jit-dump-hir-passes-json",
        "PYTHONJITDUMPHIRPASSESJSON",
        [](std::string json_output_dir) {
          g_dump_hir_passes_json = ::strdup(json_output_dir.c_str());
          int mkdir_result = ::mkdir(g_dump_hir_passes_json, 0755);
          JIT_CHECK(
              mkdir_result == 0 || errno == EEXIST,
              "could not make JSON directory");
        },
        "Dump IR passes as JSON to the directory specified by this flag's "
        "value");
    xarg_flag_processor.addOption(
        "jit-multiple-code-sections",
        "PYTHONJITMULTIPLECODESECTIONS",
        [](int val) {
          if (use_jit) {
            jit_config.multiple_code_sections = val;
          }
        },
        "Enable emitting code into multiple code sections.");

    xarg_flag_processor.addOption(
        "jit-hot-code-section-size",
        "PYTHONJITHOTCODESECTIONSIZE",
        [](size_t val) {
          if (use_jit) {
            jit_config.hot_code_section_size = val;
          }
        },
        "Enable emitting code into multiple code sections.");

    xarg_flag_processor.addOption(
        "jit-cold-code-section-size",
        "PYTHONJITCOLDCODESECTIONSIZE",
        [](size_t val) {
          if (use_jit) {
            jit_config.cold_code_section_size = val;
          }
        },
        "Enable emitting code into multiple code sections.");

    xarg_flag_processor.addOption(
        "jit-help", "", jit_help, "print all available JIT flags and exits");
  }

  xarg_flag_processor.setFlags(PySys_GetXOptions());
}

// Compile the given compilation unit, returning the result code.
static _PyJIT_Result compileUnit(BorrowedRef<> unit) {
  if (PyFunction_Check(unit)) {
    BorrowedRef<PyFunctionObject> func(unit);
    CompilationTimer t{func};
    return _PyJITContext_CompileFunction(jit_ctx, func);
  }
  JIT_CHECK(PyCode_Check(unit), "Expected function or code object");
  BorrowedRef<PyCodeObject> code(unit);
  const CodeData& data = map_get(jit_code_data, code);
  return _PyJITContext_CompileCode(jit_ctx, data.module, code, data.globals);
}

// Compile the given function or code object with preloader from jit_preloaders
// map.
static _PyJIT_Result compilePreloaded(BorrowedRef<> unit) {
  return _PyJITContext_CompilePreloader(jit_ctx, map_get(jit_preloaders, unit));
}

static void compile_worker_thread() {
  JIT_DLOG("Started compile worker in thread %d", std::this_thread::get_id());
  BorrowedRef<> unit;
  while ((unit = g_threaded_compile_context.nextUnit()) != nullptr) {
    g_compile_workers_attempted++;
    if (compilePreloaded(unit) == PYJIT_RESULT_RETRY) {
      ThreadedCompileSerialize guard;
      g_compile_workers_retries++;
      g_threaded_compile_context.retryUnit(unit);
    }
  }
  JIT_DLOG("Finished compile worker in thread %d", std::this_thread::get_id());
}

static void multithread_compile_all() {
  JIT_CHECK(jit_ctx, "JIT not initialized");

  std::vector<BorrowedRef<>> compilation_units;
  // first we have to preload everything we are going to compile
  while (jit_reg_units.size() > 0) {
    std::vector<BorrowedRef<>> preload_units = {
        jit_reg_units.begin(), jit_reg_units.end()};
    jit_reg_units.clear();
    for (auto unit : preload_units) {
      compilation_units.push_back(unit);
      if (PyFunction_Check(unit)) {
        BorrowedRef<PyFunctionObject> func(unit);
        jit_preloaders.emplace(unit, func);
      } else {
        JIT_CHECK(PyCode_Check(unit), "Expected function or code object");
        BorrowedRef<PyCodeObject> code(unit);
        const CodeData& data = map_get(jit_code_data, code);
        jit_preloaders.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(unit),
            std::forward_as_tuple(
                code, data.globals, codeFullname(data.module, code)));
      }
    }
  }
  // Disable checks for using GIL protected data across threads.
  // Conceptually what we're doing here is saying we're taking our own
  // responsibility for managing locking of CPython runtime data structures.
  // Instead of holding the GIL to serialize execution to one thread, we're
  // holding the GIL for a group of co-operating threads which are aware of each
  // other. We still need the GIL as this protects the cooperating threads from
  // unknown other threads. Within our group of cooperating threads we can
  // safely do any read-only operations in parallel, but we grab our own lock if
  // we do a write (e.g. an incref).
  int old_gil_check_enabled = _PyGILState_check_enabled;
  _PyGILState_check_enabled = 0;

  g_threaded_compile_context.startCompile(std::move(compilation_units));
  std::vector<std::thread> worker_threads;
  JIT_CHECK(jit_config.batch_compile_workers, "Zero workers for compile");
  {
    // Hold a lock while we create threads because IG production has magic to
    // wrap pthread_create() and run Python code before threads are created.
    ThreadedCompileSerialize guard;
    for (size_t i = 0; i < jit_config.batch_compile_workers; i++) {
      worker_threads.emplace_back(compile_worker_thread);
    }
  }
  for (std::thread& worker_thread : worker_threads) {
    worker_thread.join();
  }

  std::vector<BorrowedRef<>> retry_list{
      g_threaded_compile_context.endCompile()};
  for (auto unit : retry_list) {
    compilePreloaded(unit);
  }
  _PyGILState_check_enabled = old_gil_check_enabled;
  jit_preloaders.clear();
}

static PyObject* multithreaded_compile_test(PyObject*, PyObject*) {
  if (!jit_config.multithreaded_compile_test) {
    PyErr_SetString(
        PyExc_NotImplementedError, "multithreaded_compile_test not enabled");
    return NULL;
  }
  g_compile_workers_attempted = 0;
  g_compile_workers_retries = 0;
  JIT_LOG("(Re)compiling %d units", jit_reg_units.size());
  _PyJITContext_ClearCache(jit_ctx);
  std::chrono::time_point time_start = std::chrono::steady_clock::now();
  multithread_compile_all();
  std::chrono::time_point time_end = std::chrono::steady_clock::now();
  JIT_LOG(
      "Took %d ms, compiles attempted: %d, compiles retried: %d",
      std::chrono::duration_cast<std::chrono::milliseconds>(
          time_end - time_start)
          .count(),
      g_compile_workers_attempted,
      g_compile_workers_retries);
  Py_RETURN_NONE;
}

static PyObject* is_multithreaded_compile_test_enabled(PyObject*, PyObject*) {
  if (jit_config.multithreaded_compile_test) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static PyObject*
disable_jit(PyObject* /* self */, PyObject* const* args, Py_ssize_t nargs) {
  if (nargs > 1) {
    PyErr_SetString(PyExc_TypeError, "disable expects 0 or 1 arg");
    return NULL;
  } else if (nargs == 1 && !PyBool_Check(args[0])) {
    PyErr_SetString(
        PyExc_TypeError,
        "disable expects bool indicating to compile pending functions");
    return NULL;
  }

  if (nargs == 0 || args[0] == Py_True) {
    // Compile all of the pending functions/codes before shutting down
    std::chrono::time_point start = std::chrono::steady_clock::now();
    if (jit_config.batch_compile_workers > 0) {
      multithread_compile_all();
    } else {
      std::unordered_set<BorrowedRef<>> units;
      units.swap(jit_reg_units);
      for (auto unit : units) {
        compileUnit(unit);
      }
    }
    std::chrono::time_point end = std::chrono::steady_clock::now();
    g_batch_compilation_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

    jit_code_data.clear();
  }

  _PyJIT_Disable();
  Py_RETURN_NONE;
}

static PyObject* get_batch_compilation_time_ms(PyObject*, PyObject*) {
  return PyLong_FromLong(g_batch_compilation_time_ms);
}

static PyObject* force_compile(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "force_compile expected a function");
    return NULL;
  }

  if (jit_reg_units.count(func)) {
    _PyJIT_CompileFunction((PyFunctionObject*)func);
    Py_RETURN_TRUE;
  }

  Py_RETURN_FALSE;
}

int _PyJIT_IsCompiled(PyObject* func) {
  if (jit_ctx == nullptr) {
    return 0;
  }
  JIT_DCHECK(
      PyFunction_Check(func),
      "Expected PyFunctionObject, got '%.200s'",
      Py_TYPE(func)->tp_name);

  return _PyJITContext_DidCompile(jit_ctx, func);
}

static PyObject* is_jit_compiled(PyObject* /* self */, PyObject* func) {
  int st = _PyJIT_IsCompiled(func);
  PyObject* res = NULL;
  if (st == 1) {
    res = Py_True;
  } else if (st == 0) {
    res = Py_False;
  }
  Py_XINCREF(res);
  return res;
}

static PyObject* print_hir(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return NULL;
  }

  int st = _PyJITContext_DidCompile(jit_ctx, func);
  if (st == -1) {
    return NULL;
  } else if (st == 0) {
    PyErr_SetString(PyExc_ValueError, "function is not jit compiled");
    return NULL;
  }

  if (_PyJITContext_PrintHIR(jit_ctx, func) < 0) {
    return NULL;
  } else {
    Py_RETURN_NONE;
  }
}

static PyObject* disassemble(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return NULL;
  }

  int st = _PyJITContext_DidCompile(jit_ctx, func);
  if (st == -1) {
    return NULL;
  } else if (st == 0) {
    PyErr_SetString(PyExc_ValueError, "function is not jit compiled");
    return NULL;
  }

  if (_PyJITContext_Disassemble(jit_ctx, func) < 0) {
    return NULL;
  } else {
    Py_RETURN_NONE;
  }
}

static PyObject* get_jit_list(PyObject* /* self */, PyObject*) {
  if (g_jit_list == nullptr) {
    Py_RETURN_NONE;
  } else {
    auto jit_list = Ref<>::steal(g_jit_list->getList());
    return jit_list.release();
  }
}

static PyObject* jit_list_append(PyObject* /* self */, PyObject* line) {
  if (g_jit_list == nullptr) {
    g_jit_list = JITList::create().release();
  }
  Py_ssize_t line_len;
  const char* line_str = PyUnicode_AsUTF8AndSize(line, &line_len);
  if (line_str == NULL) {
    return NULL;
  }
  g_jit_list->parseLine(
      {line_str, static_cast<std::string::size_type>(line_len)});
  Py_RETURN_NONE;
}

static PyObject* get_compiled_functions(PyObject* /* self */, PyObject*) {
  return _PyJITContext_GetCompiledFunctions(jit_ctx);
}

static PyObject* get_compilation_time(PyObject* /* self */, PyObject*) {
  PyObject* res =
      PyLong_FromLong(static_cast<long>(total_compliation_time * 1000));
  return res;
}

static PyObject* get_function_compilation_time(
    PyObject* /* self */,
    PyObject* func) {
  auto iter =
      jit_time_functions.find(reinterpret_cast<PyFunctionObject*>(func));
  if (iter == jit_time_functions.end()) {
    Py_RETURN_NONE;
  }

  PyObject* res = PyLong_FromLong(iter->second.count() * 1000);
  return res;
}

static PyObject* get_num_inlined_functions(PyObject*, PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }
  int size = _PyJITContext_GetNumInlinedFunctions(jit_ctx, func);
  return PyLong_FromLong(size);
}

namespace {

// Simple wrapper functions to turn NULL or -1 return values from C-API
// functions into a thrown exception. Meant for repetitive runs of C-API calls
// and not intended for use in public APIs.
class CAPIError : public std::exception {};

PyObject* check(PyObject* obj) {
  if (obj == nullptr) {
    throw CAPIError();
  }
  return obj;
}

int check(int ret) {
  if (ret < 0) {
    throw CAPIError();
  }
  return ret;
}

Ref<> make_deopt_stats() {
  Runtime* runtime = Runtime::get();
  auto stats = Ref<>::steal(check(PyList_New(0)));

  for (auto& pair : runtime->deoptStats()) {
    const DeoptMetadata& meta = runtime->getDeoptMetadata(pair.first);
    const DeoptFrameMetadata& frame_meta = meta.frame_meta[meta.inline_depth];
    const DeoptStat& stat = pair.second;
    BorrowedRef<PyCodeObject> code = frame_meta.code;

    auto func_qualname = code->co_qualname;
    int lineno_raw = code->co_lnotab != nullptr
        ? PyCode_Addr2Line(code, frame_meta.next_instr_offset)
        : -1;
    auto lineno = Ref<>::steal(check(PyLong_FromLong(lineno_raw)));
    auto reason =
        Ref<>::steal(check(PyUnicode_FromString(deoptReasonName(meta.reason))));
    auto description = Ref<>::steal(check(PyUnicode_FromString(meta.descr)));

    // Helper to create an event dict with a given count value.
    auto append_event = [&](size_t count_raw, const char* type) {
      auto event = Ref<>::steal(check(PyDict_New()));
      auto normals = Ref<>::steal(check(PyDict_New()));
      auto ints = Ref<>::steal(check(PyDict_New()));

      check(PyDict_SetItem(event, s_str_normal, normals));
      check(PyDict_SetItem(event, s_str_int, ints));
      check(PyDict_SetItem(normals, s_str_func_qualname, func_qualname));
      check(PyDict_SetItem(normals, s_str_filename, code->co_filename));
      check(PyDict_SetItem(ints, s_str_lineno, lineno));
      check(PyDict_SetItem(normals, s_str_reason, reason));
      check(PyDict_SetItem(normals, s_str_description, description));

      auto count = Ref<>::steal(check(PyLong_FromSize_t(count_raw)));
      check(PyDict_SetItem(ints, s_str_count, count));
      auto type_str = Ref<>::steal(check(PyUnicode_InternFromString(type)));
      check(PyDict_SetItem(normals, s_str_guilty_type, type_str) < 0);
      check(PyList_Append(stats, event));
    };

    // For deopts with type profiles, add a copy of the dict with counts for
    // each type, including "other".
    if (!stat.types.empty()) {
      for (size_t i = 0; i < stat.types.size && stat.types.types[i] != nullptr;
           ++i) {
        append_event(stat.types.counts[i], stat.types.types[i]->tp_name);
      }
      if (stat.types.other > 0) {
        append_event(stat.types.other, "<other>");
      }
    } else {
      append_event(stat.count, "<none>");
    }
  }

  runtime->clearDeoptStats();

  return stats;
}

} // namespace

static PyObject* get_and_clear_runtime_stats(PyObject* /* self */, PyObject*) {
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  try {
    Ref<> deopt_stats = make_deopt_stats();
    check(PyDict_SetItemString(stats, "deopt", deopt_stats));
  } catch (const CAPIError&) {
    return nullptr;
  }

  return stats.release();
}

static PyObject* clear_runtime_stats(PyObject* /* self */, PyObject*) {
  Runtime::get()->clearDeoptStats();
  Py_RETURN_NONE;
}

static PyObject* get_compiled_size(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }

  long size = _PyJITContext_GetCodeSize(jit_ctx, func);
  PyObject* res = PyLong_FromLong(size);
  return res;
}

static PyObject* get_compiled_stack_size(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }

  long size = _PyJITContext_GetStackSize(jit_ctx, func);
  PyObject* res = PyLong_FromLong(size);
  return res;
}

static PyObject* get_compiled_spill_stack_size(
    PyObject* /* self */,
    PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }

  long size = _PyJITContext_GetSpillStackSize(jit_ctx, func);
  PyObject* res = PyLong_FromLong(size);
  return res;
}

static PyObject* jit_frame_mode(PyObject* /* self */, PyObject*) {
  return PyLong_FromLong(jit_config.frame_mode);
}

static PyObject* get_supported_opcodes(PyObject* /* self */, PyObject*) {
  auto set = Ref<>::steal(PySet_New(nullptr));
  if (set == nullptr) {
    return nullptr;
  }

  for (auto op : hir::kSupportedOpcodes) {
    auto op_obj = Ref<>::steal(PyLong_FromLong(op));
    if (op_obj == nullptr) {
      return nullptr;
    }
    if (PySet_Add(set, op_obj) < 0) {
      return nullptr;
    }
  }

  return set.release();
}

static PyObject* jit_force_normal_frame(PyObject*, PyObject* func_obj) {
  if (!PyFunction_Check(func_obj)) {
    PyErr_SetString(PyExc_TypeError, "Input must be a function");
    return NULL;
  }
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(func_obj);

  reinterpret_cast<PyCodeObject*>(func->func_code)->co_flags |= CO_NORMAL_FRAME;

  Py_INCREF(func_obj);
  return func_obj;
}

static PyObject* jit_suppress(PyObject*, PyObject* func_obj) {
  if (!PyFunction_Check(func_obj)) {
    PyErr_SetString(PyExc_TypeError, "Input must be a function");
    return NULL;
  }
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(func_obj);

  reinterpret_cast<PyCodeObject*>(func->func_code)->co_flags |= CO_SUPPRESS_JIT;

  Py_INCREF(func_obj);
  return func_obj;
}

static PyObject* get_allocator_stats(PyObject*, PyObject*) {
  if (!_PyJIT_UseHugePages()) {
    Py_RETURN_NONE;
  }
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == NULL) {
    return NULL;
  }
  auto used_bytes =
      Ref<>::steal(PyLong_FromLong(CodeAllocatorCinder::usedBytes()));
  if (used_bytes == NULL ||
      PyDict_SetItemString(stats, "used_bytes", used_bytes) < 0) {
    return NULL;
  }
  auto lost_bytes =
      Ref<>::steal(PyLong_FromLong(CodeAllocatorCinder::lostBytes()));
  if (lost_bytes == NULL ||
      PyDict_SetItemString(stats, "lost_bytes", lost_bytes) < 0) {
    return NULL;
  }
  auto fragmented_allocs =
      Ref<>::steal(PyLong_FromLong(CodeAllocatorCinder::fragmentedAllocs()));
  if (fragmented_allocs == NULL ||
      PyDict_SetItemString(stats, "fragmented_allocs", fragmented_allocs) < 0) {
    return NULL;
  }
  auto huge_allocs =
      Ref<>::steal(PyLong_FromLong(CodeAllocatorCinder::hugeAllocs()));
  if (huge_allocs == NULL ||
      PyDict_SetItemString(stats, "huge_allocs", huge_allocs) < 0) {
    return NULL;
  }
  return stats.release();
}

static PyObject* is_hir_inliner_enabled(PyObject* /* self */, PyObject*) {
  int result = _PyJIT_IsHIRInlinerEnabled();
  if (result) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static PyObject* enable_hir_inliner(PyObject* /* self */, PyObject*) {
  _PyJIT_EnableHIRInliner();
  Py_RETURN_NONE;
}

static PyObject* disable_hir_inliner(PyObject* /* self */, PyObject*) {
  _PyJIT_DisableHIRInliner();
  Py_RETURN_NONE;
}

static PyMethodDef jit_methods[] = {
    {"disable",
     (PyCFunction)(void*)disable_jit,
     METH_FASTCALL,
     "Disable the jit."},
    {"disassemble", disassemble, METH_O, "Disassemble JIT compiled functions"},
    {"is_jit_compiled",
     is_jit_compiled,
     METH_O,
     "Check if a function is jit compiled."},
    {"force_compile",
     force_compile,
     METH_O,
     "Force a function to be JIT compiled if it hasn't yet"},
    {"jit_frame_mode",
     jit_frame_mode,
     METH_NOARGS,
     "Get JIT frame mode (0 = normal frames, 1 = no frames, 2 = shadow frames"},
    {"get_jit_list", get_jit_list, METH_NOARGS, "Get the JIT-list"},
    {"jit_list_append", jit_list_append, METH_O, "Parse a JIT-list line"},
    {"print_hir",
     print_hir,
     METH_O,
     "Print the HIR for a jitted function to stdout."},
    {"get_supported_opcodes",
     get_supported_opcodes,
     METH_NOARGS,
     "Return a set of all supported opcodes, as ints."},
    {"get_compiled_functions",
     get_compiled_functions,
     METH_NOARGS,
     "Return a list of functions that are currently JIT-compiled."},
    {"get_compilation_time",
     get_compilation_time,
     METH_NOARGS,
     "Return the total time used for JIT compiling functions in milliseconds."},
    {"get_function_compilation_time",
     get_function_compilation_time,
     METH_O,
     "Return the time used for JIT compiling a given function in "
     "milliseconds."},
    {"get_and_clear_runtime_stats",
     get_and_clear_runtime_stats,
     METH_NOARGS,
     "Returns information about the runtime behavior of JIT-compiled code."},
    {"clear_runtime_stats",
     clear_runtime_stats,
     METH_NOARGS,
     "Clears runtime stats about JIT-compiled code without returning a value."},
    {"get_compiled_size",
     get_compiled_size,
     METH_O,
     "Return code size in bytes for a JIT-compiled function."},
    {"get_compiled_stack_size",
     get_compiled_stack_size,
     METH_O,
     "Return stack size in bytes for a JIT-compiled function."},
    {"get_compiled_spill_stack_size",
     get_compiled_spill_stack_size,
     METH_O,
     "Return stack size in bytes used for register spills for a JIT-compiled "
     "function."},
    {"jit_force_normal_frame",
     jit_force_normal_frame,
     METH_O,
     "Decorator forcing a function to always use normal frame mode when JIT."},
    {"jit_suppress",
     jit_suppress,
     METH_O,
     "Decorator to disable the JIT for the decorated function."},
    {"multithreaded_compile_test",
     multithreaded_compile_test,
     METH_NOARGS,
     "Force multi-threaded recompile of still existing JIT functions for test"},
    {"is_multithreaded_compile_test_enabled",
     is_multithreaded_compile_test_enabled,
     METH_NOARGS,
     "Return True if multithreaded_compile_test mode is enabled"},
    {"get_batch_compilation_time_ms",
     get_batch_compilation_time_ms,
     METH_NOARGS,
     "Return the number of milliseconds spent in batch compilation when "
     "disabling the JIT."},
    {"get_allocator_stats",
     get_allocator_stats,
     METH_NOARGS,
     "Return stats from the code allocator as a dictionary."},
    {"is_hir_inliner_enabled",
     is_hir_inliner_enabled,
     METH_NOARGS,
     "Return True if the HIR inliner is enabled and False otherwise."},
    {"enable_hir_inliner",
     enable_hir_inliner,
     METH_NOARGS,
     "Enable the HIR inliner."},
    {"disable_hir_inliner",
     disable_hir_inliner,
     METH_NOARGS,
     "Disable the HIR inliner."},
    {"get_num_inlined_functions",
     get_num_inlined_functions,
     METH_O,
     "Return the number of inline sites in this function."},
    {NULL, NULL, 0, NULL}};

static PyModuleDef jit_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "cinderjit",
    .m_doc = NULL,
    .m_size = -1,
    .m_methods = jit_methods,
    .m_slots = nullptr,
    .m_traverse = nullptr,
    .m_clear = nullptr,
    .m_free = nullptr};

static int onJitListImpl(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<> mod,
    BorrowedRef<> qualname) {
  bool is_static = code->co_flags & CO_STATICALLY_COMPILED;
  if (g_jit_list == nullptr ||
      (is_static && jit_config.compile_all_static_functions)) {
    // There's no jit list or the function is static.
    return 1;
  }
  if (g_jit_list->lookupCO(code) != 1) {
    return g_jit_list->lookupFO(mod, qualname);
  }
  return 1;
}

int _PyJIT_OnJitList(PyFunctionObject* func) {
  return onJitListImpl(func->func_code, func->func_module, func->func_qualname);
}

int _PyJIT_Initialize() {
  if (jit_config.init_state == JIT_INITIALIZED) {
    return 0;
  }

  initJitConfig_();

  // Initialize some interned strings that can be used even when the JIT is
  // off.
#define INTERN_STR(s)                         \
  s_str_##s = PyUnicode_InternFromString(#s); \
  if (s_str_##s == nullptr) {                 \
    return -1;                                \
  }
  INTERNED_STRINGS(INTERN_STR)
#undef INTERN_STR

#define MAKE_OPNAME(opname, opnum)                                   \
  if ((s_opnames.at(opnum) = PyUnicode_InternFromString(#opname)) == \
      nullptr) {                                                     \
    return -1;                                                       \
  }
  PY_OPCODES(MAKE_OPNAME)
#undef MAKE_OPNAME

  initFlagProcessor();

  if (jit_help) {
    std::cout << xarg_flag_processor.jitXOptionHelpMessage() << std::endl;
    return -2;
  }

  std::unique_ptr<JITList> jit_list;
  if (!jl_fn.empty()) {
    if (jit_config.allow_jit_list_wildcards) {
      jit_list = jit::WildcardJITList::create();
    } else {
      jit_list = jit::JITList::create();
    }
    if (jit_list == nullptr) {
      JIT_LOG("Failed to allocate JIT list");
      return -1;
    }
    if (!jit_list->parseFile(jl_fn.c_str())) {
      JIT_LOG("Could not parse jit-list, disabling JIT.");
      return 0;
    }
  }

  if (!write_profile_file.empty() || jit_profile_interp == 1) {
    if (use_jit) {
      use_jit = 0;
      JIT_LOG("Keeping JIT disabled to enable interpreter profiling.");
    }
    g_profile_new_interp_threads = 1;
    _PyThreadState_SetProfileInterpAll(1);
    if (!write_profile_file.empty()) {
      g_write_profile_file = write_profile_file;
    }
  }

  if (use_jit) {
    JIT_DLOG("Enabling JIT.");
  } else {
    return 0;
  }

  CodeAllocator::makeGlobalCodeAllocator();

  jit_ctx = new _PyJITContext();

  PyObject* mod = PyModule_Create(&jit_module);
  if (mod == NULL) {
    return -1;
  }

  PyObject* modname = PyUnicode_InternFromString("cinderjit");
  if (modname == NULL) {
    return -1;
  }

  PyObject* modules = PyImport_GetModuleDict();
  int st = _PyImport_FixupExtensionObject(mod, modname, modname, modules);
  Py_DECREF(modname);
  if (st == -1) {
    return -1;
  }

  jit_config.init_state = JIT_INITIALIZED;
  jit_config.is_enabled = 1;
  g_jit_list = jit_list.release();
  // Unconditionally set this, since we might have shadow frames from
  // CO_SHADOW_FRAME or inlined functions.
  _PyThreadState_GetFrame =
      reinterpret_cast<PyThreadFrameGetter>(materializeShadowCallStack);

  total_compliation_time = 0.0;

  return 0;
}

bool _PyJIT_UseHugePages() {
  return jit_config.use_huge_pages;
}

int _PyJIT_IsEnabled() {
  return (jit_config.init_state == JIT_INITIALIZED) && jit_config.is_enabled;
}

void _PyJIT_AfterFork_Child() {
  perf::afterForkChild();
}

int _PyJIT_AreTypeSlotsEnabled() {
  return (jit_config.init_state == JIT_INITIALIZED) &&
      jit_config.are_type_slots_enabled;
}

void _PyJIT_EnableHIRInliner() {
  jit_config.hir_inliner_enabled = 1;
}

void _PyJIT_DisableHIRInliner() {
  jit_config.hir_inliner_enabled = 0;
}

int _PyJIT_IsHIRInlinerEnabled() {
  return jit_config.hir_inliner_enabled;
}

int _PyJIT_MultipleCodeSectionsEnabled() {
  return jit_config.multiple_code_sections;
}

int _PyJIT_HotCodeSectionSize() {
  if (!_PyJIT_MultipleCodeSectionsEnabled()) {
    return 0;
  }
  return jit_config.hot_code_section_size;
}

int _PyJIT_ColdCodeSectionSize() {
  if (!_PyJIT_MultipleCodeSectionsEnabled()) {
    return 0;
  }
  return jit_config.cold_code_section_size;
}

int _PyJIT_Enable() {
  if (jit_config.init_state != JIT_INITIALIZED) {
    return 0;
  }
  jit_config.is_enabled = 1;
  return 0;
}

int _PyJIT_EnableTypeSlots() {
  if (!_PyJIT_IsEnabled()) {
    return 0;
  }
  jit_config.are_type_slots_enabled = 1;
  return 1;
}

void _PyJIT_Disable() {
  jit_config.is_enabled = 0;
  jit_config.are_type_slots_enabled = 0;
}

_PyJIT_Result _PyJIT_SpecializeType(
    PyTypeObject* type,
    _PyJIT_TypeSlots* slots) {
  return _PyJITContext_SpecializeType(jit_ctx, type, slots);
}

_PyJIT_Result _PyJIT_CompileFunction(PyFunctionObject* func) {
  if (jit_ctx == nullptr) {
    return PYJIT_NOT_INITIALIZED;
  }

  if (g_threaded_compile_context.compileRunning()) {
    // we were called recursively (by emitInvokeFunction);
    // find preloader in global map and compile it.

    auto it = jit_preloaders.find(func->func_code);
    if (it == jit_preloaders.end()) {
      return PYJIT_RESULT_CANNOT_SPECIALIZE;
    }
    return _PyJITContext_CompilePreloader(jit_ctx, it->second);
  }

  if (!_PyJIT_OnJitList(func)) {
    return PYJIT_RESULT_CANNOT_SPECIALIZE;
  }

  CompilationTimer timer(func);
  jit_reg_units.erase(reinterpret_cast<PyObject*>(func));
  return _PyJITContext_CompileFunction(jit_ctx, func);
}

// Recursively search the given co_consts tuple for any code objects that are
// on the current jit-list, using the given module name to form a
// fully-qualified function name.
static std::vector<BorrowedRef<PyCodeObject>> findNestedCodes(
    BorrowedRef<> module,
    BorrowedRef<> root_consts) {
  std::queue<PyObject*> consts_tuples;
  std::unordered_set<PyCodeObject*> visited;
  std::vector<BorrowedRef<PyCodeObject>> result;

  consts_tuples.push(root_consts);
  while (!consts_tuples.empty()) {
    PyObject* consts = consts_tuples.front();
    consts_tuples.pop();

    for (size_t i = 0, size = PyTuple_GET_SIZE(consts); i < size; ++i) {
      BorrowedRef<PyCodeObject> code = PyTuple_GET_ITEM(consts, i);
      if (!PyCode_Check(code) || !visited.insert(code).second ||
          code->co_qualname == nullptr ||
          !onJitListImpl(code, module, code->co_qualname)) {
        continue;
      }

      result.emplace_back(code);
      consts_tuples.emplace(code->co_consts);
    }
  }

  return result;
}

int _PyJIT_RegisterFunction(PyFunctionObject* func) {
  // Attempt to attach already-compiled code even if the JIT is disabled, as
  // long as it hasn't been finalized.
  if (jit_ctx != nullptr &&
      _PyJITContext_AttachCompiledCode(jit_ctx, func) == PYJIT_RESULT_OK) {
    return 1;
  }

  if (!_PyJIT_IsEnabled()) {
    return 0;
  }

  JIT_CHECK(
      !g_threaded_compile_context.compileRunning(),
      "Not intended for using during threaded compilation");
  int result = 0;
  if (_PyJIT_OnJitList(func)) {
    jit_reg_units.emplace(reinterpret_cast<PyObject*>(func));
    result = 1;
  }

  // If we have an active jit-list, scan this function's code object for any
  // nested functions that might be on the jit-list, and register them as
  // well.
  if (g_jit_list != nullptr) {
    PyObject* module = func->func_module;
    PyObject* globals = func->func_globals;
    for (auto code : findNestedCodes(
             module,
             reinterpret_cast<PyCodeObject*>(func->func_code)->co_consts)) {
      jit_reg_units.emplace(reinterpret_cast<PyObject*>(code.get()));
      jit_code_data.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(code),
          std::forward_as_tuple(module, globals));
    }
  }
  return result;
}

void _PyJIT_TypeCreated(PyTypeObject* type) {
  registerProfiledType(type);
}

void _PyJIT_TypeModified(PyTypeObject* type) {
  if (jit_ctx) {
    _PyJITContext_TypeModified(jit_ctx, type);
  }
  jit::notifyICsTypeChanged(type);
}

void _PyJIT_TypeNameModified(PyTypeObject* type) {
  // We assume that this is a very rare case, and simply give up on tracking
  // the type if it happens.
  unregisterProfiledType(type);
}

void _PyJIT_TypeDestroyed(PyTypeObject* type) {
  if (jit_ctx) {
    _PyJITContext_TypeDestroyed(jit_ctx, type);
  }
  unregisterProfiledType(type);
}

void _PyJIT_FuncModified(PyFunctionObject* func) {
  if (jit_ctx) {
    _PyJITContext_FuncModified(jit_ctx, func);
  }
}

void _PyJIT_FuncDestroyed(PyFunctionObject* func) {
  if (_PyJIT_IsEnabled()) {
    jit_reg_units.erase(reinterpret_cast<PyObject*>(func));
  }
  if (jit_ctx) {
    _PyJITContext_FuncDestroyed(jit_ctx, func);
  }
}

void _PyJIT_CodeDestroyed(PyCodeObject* code) {
  if (_PyJIT_IsEnabled()) {
    jit_reg_units.erase(reinterpret_cast<PyObject*>(code));
    jit_code_data.erase(code);
  }
}

static void dump_jit_stats() {
  auto stats = get_and_clear_runtime_stats(nullptr, nullptr);
  if (stats == nullptr) {
    return;
  }
  auto stats_str = PyObject_Str(stats);
  if (stats_str == nullptr) {
    return;
  }

  JIT_LOG("JIT runtime stats:\n%s", PyUnicode_AsUTF8(stats_str));
  Py_DECREF(stats_str);
}

int _PyJIT_Finalize() {
  if (g_dump_stats) {
    dump_jit_stats();
  }

  if (!g_write_profile_file.empty()) {
    writeProfileData(g_write_profile_file.c_str());
    g_write_profile_file.clear();
  }
  clearProfileData();

  // Always release references from Runtime objects: C++ clients may have
  // invoked the JIT directly without initializing a full _PyJITContext.
  jit::Runtime::get()->clearDeoptStats();
  jit::Runtime::get()->releaseReferences();

  if (jit_config.init_state == JIT_INITIALIZED) {
    delete g_jit_list;
    g_jit_list = nullptr;

    jit_config.init_state = JIT_FINALIZED;

    JIT_CHECK(jit_ctx != nullptr, "jit_ctx not initialized");
    delete jit_ctx;
    jit_ctx = nullptr;

    CodeAllocator::freeGlobalCodeAllocator();
  }

#define CLEAR_STR(s) Py_CLEAR(s_str_##s);
  INTERNED_STRINGS(CLEAR_STR)
#undef CLEAR_STR
  for (PyObject*& opname : s_opnames) {
    if (opname != nullptr) {
      Py_DECREF(opname);
      opname = nullptr;
    }
  }

  Runtime::shutdown();
  return 0;
}

int _PyJIT_ShadowFrame() {
  return jit_config.frame_mode == SHADOW_FRAME;
}

PyObject* _PyJIT_GenSend(
    PyGenObject* gen,
    PyObject* arg,
    int exc,
    PyFrameObject* f,
    PyThreadState* tstate,
    int finish_yield_from) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);

  // state should be valid and the generator should not be completed
  JIT_DCHECK(
      gen_footer->state == _PyJitGenState_JustStarted ||
          gen_footer->state == _PyJitGenState_Running,
      "Invalid JIT generator state");

  gen_footer->state = _PyJitGenState_Running;

  // JIT generators use NULL arg to indicate an exception
  if (exc) {
    JIT_DCHECK(
        arg == Py_None, "Arg should be None when injecting an exception");
    arg = NULL;
  } else {
    if (arg == NULL) {
      arg = Py_None;
    }
  }

  if (f) {
    // Setup tstate/frame as would be done in PyEval_EvalFrameEx() or
    // prologue of a JITed function.
    tstate->frame = f;
    f->f_executing = 1;
    // This compensates for the decref which occurs in JITRT_UnlinkFrame().
    Py_INCREF(f);
    // This satisfies code which uses f_lasti == -1 or < 0 to check if a
    // generator is not yet started, but still provides a garbage value in case
    // anything tries to actually use f_lasti.
    f->f_lasti = std::numeric_limits<int>::max();
  }

  // Enter generated code.
  JIT_DCHECK(
      gen_footer->yieldPoint != nullptr,
      "Attempting to resume a generator with no yield point");
  PyObject* result =
      gen_footer->resumeEntry((PyObject*)gen, arg, tstate, finish_yield_from);

  if (!result && (gen->gi_jit_data != nullptr)) {
    // Generator jit data (gen_footer) will be freed if the generator
    // deopts
    gen_footer->state = _PyJitGenState_Completed;
  }

  return result;
}

PyFrameObject* _PyJIT_GenMaterializeFrame(PyGenObject* gen) {
  PyThreadState* tstate = PyThreadState_Get();
  PyFrameObject* frame = jit::materializePyFrameForGen(tstate, gen);
  return frame;
}

int _PyJIT_GenVisitRefs(PyGenObject* gen, visitproc visit, void* arg) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  if (gen_footer->state != _PyJitGenState_Completed && gen_footer->yieldPoint) {
    return reinterpret_cast<GenYieldPoint*>(gen_footer->yieldPoint)
        ->visitRefs(gen, visit, arg);
  }
  return 0;
}

void _PyJIT_GenDealloc(PyGenObject* gen) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  if (gen_footer->state != _PyJitGenState_Completed && gen_footer->yieldPoint) {
    reinterpret_cast<GenYieldPoint*>(gen_footer->yieldPoint)->releaseRefs(gen);
  }
  JITRT_GenJitDataFree(gen);
}

PyObject* _PyJIT_GenYieldFromValue(PyGenObject* gen) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  PyObject* yf = NULL;
  if (gen_footer->state != _PyJitGenState_Completed && gen_footer->yieldPoint) {
    yf = gen_footer->yieldPoint->yieldFromValue(gen_footer);
    Py_XINCREF(yf);
  }
  return yf;
}

PyObject* _PyJIT_GetGlobals(PyThreadState* tstate) {
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;
  if (shadow_frame == nullptr) {
    JIT_CHECK(
        tstate->frame == nullptr,
        "py frame w/out corresponding shadow frame\n");
    return nullptr;
  }
  if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
    return _PyShadowFrame_GetPyFrame(shadow_frame)->f_globals;
  }
  if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_RTFS) {
    return static_cast<jit::RuntimeFrameState*>(
               _PyShadowFrame_GetPtr(shadow_frame))
        ->globals();
  }
  // TODO(T110700318): Collapse into RTFS case
  JIT_DCHECK(
      _PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_CODE_RT,
      "Unexpected shadow frame type");
  jit::CodeRuntime* code_rt =
      static_cast<jit::CodeRuntime*>(_PyShadowFrame_GetPtr(shadow_frame));
  return code_rt->frameState()->globals();
}

void _PyJIT_ProfileCurrentInstr(
    PyFrameObject* frame,
    PyObject** stack_top,
    int opcode,
    int oparg) {
  auto profile_stack = [&](auto... stack_offsets) {
    CodeProfile& code_profile =
        jit::Runtime::get()->typeProfiles()[Ref<PyCodeObject>{frame->f_code}];
    int opcode_offset = frame->f_lasti;

    auto pair = code_profile.typed_hits.emplace(opcode_offset, nullptr);
    if (pair.second) {
      constexpr int kProfilerRows = 4;
      pair.first->second =
          TypeProfiler::create(kProfilerRows, sizeof...(stack_offsets));
    }
    auto get_type = [&](int offset) {
      PyObject* obj = stack_top[-(offset + 1)];
      return obj != nullptr ? Py_TYPE(obj) : nullptr;
    };
    pair.first->second->recordTypes(get_type(stack_offsets)...);
  };

  switch (opcode) {
    case BEFORE_ASYNC_WITH:
    case DELETE_ATTR:
    case END_ASYNC_FOR:
    case END_FINALLY:
    case FOR_ITER:
    case GET_AITER:
    case GET_ANEXT:
    case GET_AWAITABLE:
    case GET_ITER:
    case GET_YIELD_FROM_ITER:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_TRUE_OR_POP:
    case LOAD_ATTR:
    case LOAD_FIELD:
    case LOAD_METHOD:
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_TRUE:
    case RETURN_VALUE:
    case SETUP_WITH:
    case STORE_DEREF:
    case STORE_GLOBAL:
    case UNARY_INVERT:
    case UNARY_NEGATIVE:
    case UNARY_NOT:
    case UNARY_POSITIVE:
    case UNPACK_EX:
    case UNPACK_SEQUENCE:
    case WITH_CLEANUP_START:
    case YIELD_FROM:
    case YIELD_VALUE: {
      profile_stack(0);
      break;
    }
    case BINARY_ADD:
    case BINARY_AND:
    case BINARY_FLOOR_DIVIDE:
    case BINARY_LSHIFT:
    case BINARY_MATRIX_MULTIPLY:
    case BINARY_MODULO:
    case BINARY_MULTIPLY:
    case BINARY_OR:
    case BINARY_POWER:
    case BINARY_RSHIFT:
    case BINARY_SUBSCR:
    case BINARY_SUBTRACT:
    case BINARY_TRUE_DIVIDE:
    case BINARY_XOR:
    case COMPARE_OP:
    case DELETE_SUBSCR:
    case INPLACE_ADD:
    case INPLACE_AND:
    case INPLACE_FLOOR_DIVIDE:
    case INPLACE_LSHIFT:
    case INPLACE_MATRIX_MULTIPLY:
    case INPLACE_MODULO:
    case INPLACE_MULTIPLY:
    case INPLACE_OR:
    case INPLACE_POWER:
    case INPLACE_RSHIFT:
    case INPLACE_SUBTRACT:
    case INPLACE_TRUE_DIVIDE:
    case INPLACE_XOR:
    case LIST_APPEND:
    case MAP_ADD:
    case SET_ADD:
    case STORE_ATTR:
    case STORE_FIELD:
    case WITH_CLEANUP_FINISH: {
      profile_stack(1, 0);
      break;
    }
    case STORE_SUBSCR: {
      profile_stack(2, 1, 0);
      break;
    }
    case CALL_FUNCTION: {
      profile_stack(oparg);
      break;
    };
    case CALL_METHOD: {
      profile_stack(oparg, oparg + 1);
      break;
    }
  }
}

void _PyJIT_CountProfiledInstrs(PyCodeObject* code, Py_ssize_t count) {
  jit::Runtime::get()->typeProfiles()[Ref<PyCodeObject>{code}].total_hits +=
      count;
}

namespace {

// ProfileEnv and the functions below that use it are for building the
// complicated, nested data structure returned by
// _PyJIT_GetAndClearTypeProfiles().
struct ProfileEnv {
  // These members are applicable during the whole process:
  Ref<> stats_list;
  Ref<> other_list;
  Ref<> empty_list;
  UnorderedMap<BorrowedRef<PyTypeObject>, Ref<>> type_name_cache;

  // These members vary with each code object:
  BorrowedRef<PyCodeObject> code;
  Ref<> code_hash;
  Ref<> qualname;
  Ref<> firstlineno;

  // These members vary with each instruction:
  int64_t profiled_hits;
  Ref<> bc_offset;
  Ref<> opname;
  Ref<> lineno;
};

void init_env(ProfileEnv& env) {
  env.stats_list = Ref<>::steal(check(PyList_New(0)));
  env.other_list = Ref<>::steal(check(PyList_New(0)));
  auto other_str = Ref<>::steal(check(PyUnicode_InternFromString("<other>")));
  check(PyList_Append(env.other_list, other_str));
  env.empty_list = Ref<>::steal(check(PyList_New(0)));

  env.type_name_cache.emplace(
      nullptr, Ref<>::steal(check(PyUnicode_InternFromString("<NULL>"))));
}

PyObject* get_type_name(ProfileEnv& env, PyTypeObject* ty) {
  auto pair = env.type_name_cache.emplace(ty, nullptr);
  Ref<>& cached_name = pair.first->second;
  if (pair.second) {
    cached_name = Ref<>::steal(
        check(PyUnicode_InternFromString(typeFullname(ty).c_str())));
  }
  return cached_name;
}

void start_code(ProfileEnv& env, PyCodeObject* code) {
  env.code = code;
  env.code_hash =
      Ref<>::steal(check(PyLong_FromUnsignedLong(hashBytecode(code))));
  env.qualname = Ref<>::steal(
      check(PyUnicode_InternFromString(codeQualname(code).c_str())));
  env.firstlineno = Ref<>::steal(check(PyLong_FromLong(code->co_firstlineno)));
  env.profiled_hits = 0;
}

void start_instr(ProfileEnv& env, int bcoff_raw) {
  int lineno_raw = env.code->co_lnotab != nullptr
      ? PyCode_Addr2Line(env.code, bcoff_raw)
      : -1;
  int opcode = _Py_OPCODE(PyBytes_AS_STRING(env.code->co_code)[bcoff_raw]);
  env.bc_offset = Ref<>::steal(check(PyLong_FromLong(bcoff_raw)));
  env.lineno = Ref<>::steal(check(PyLong_FromLong(lineno_raw)));
  env.opname.reset(s_opnames.at(opcode));
}

void append_item(
    ProfileEnv& env,
    long count_raw,
    PyObject* type_names,
    bool use_op = true) {
  auto item = Ref<>::steal(check(PyDict_New()));
  auto normals = Ref<>::steal(check(PyDict_New()));
  auto ints = Ref<>::steal(check(PyDict_New()));
  auto count = Ref<>::steal(check(PyLong_FromLong(count_raw)));

  check(PyDict_SetItem(item, s_str_normal, normals));
  check(PyDict_SetItem(item, s_str_int, ints));
  check(PyDict_SetItem(normals, s_str_func_qualname, env.qualname));
  check(PyDict_SetItem(normals, s_str_filename, env.code->co_filename));
  check(PyDict_SetItem(ints, s_str_code_hash, env.code_hash));
  check(PyDict_SetItem(ints, s_str_firstlineno, env.firstlineno));
  check(PyDict_SetItem(ints, s_str_count, count));
  if (use_op) {
    check(PyDict_SetItem(ints, s_str_lineno, env.lineno));
    check(PyDict_SetItem(ints, s_str_bc_offset, env.bc_offset));
    check(PyDict_SetItem(normals, s_str_opname, env.opname));
  }
  if (type_names != nullptr) {
    auto normvectors = Ref<>::steal(check(PyDict_New()));
    check(PyDict_SetItem(normvectors, s_str_types, type_names));
    check(PyDict_SetItem(item, s_str_normvector, normvectors));
  }
  check(PyList_Append(env.stats_list, item));

  env.profiled_hits += count_raw;
}

void build_profile(ProfileEnv& env, TypeProfiles& profiles) {
  for (auto& code_pair : profiles) {
    start_code(env, code_pair.first);
    const CodeProfile& code_profile = code_pair.second;

    for (auto& profile_pair : code_profile.typed_hits) {
      const TypeProfiler& profile = *profile_pair.second;
      if (profile.empty()) {
        continue;
      }
      start_instr(env, profile_pair.first);

      for (int row = 0; row < profile.rows() && profile.count(row) != 0;
           ++row) {
        auto type_names = Ref<>::steal(check(PyList_New(0)));
        for (int col = 0; col < profile.cols(); ++col) {
          PyTypeObject* ty = profile.type(row, col);
          check(PyList_Append(type_names, get_type_name(env, ty)));
        }
        append_item(env, profile.count(row), type_names);
      }

      if (profile.other() > 0) {
        append_item(env, profile.other(), env.other_list);
      }
    }

    int64_t untyped_hits = code_profile.total_hits - env.profiled_hits;
    if (untyped_hits != 0) {
      append_item(env, untyped_hits, nullptr, false);
    }
  }
}

} // namespace

PyObject* _PyJIT_GetAndClearTypeProfiles() {
  auto& profiles = jit::Runtime::get()->typeProfiles();
  ProfileEnv env;

  try {
    init_env(env);
    build_profile(env, profiles);
  } catch (const CAPIError&) {
    return nullptr;
  }

  profiles.clear();
  return env.stats_list.release();
}

void _PyJIT_ClearTypeProfiles() {
  jit::Runtime::get()->typeProfiles().clear();
}
