%module libshell

%{
#include <stddef.h>
%}

%include "typemaps.i"
%include "cpointer.i"
%include "std_string.i"

typedef struct Command {
  const char *program;
  int exit_code;
} Command;

int shell_exec(const char *command_line);
int shell_compute(int lhs, int rhs);

Command *command_new(const char *program);
void command_free(Command *cmd);
const char *command_get_program(Command *cmd);
void command_set_program(Command *cmd, const char *program);
int command_get_exit_code(Command *cmd);
void command_set_exit_code(Command *cmd, int code);

#ifdef __cplusplus
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

class ShellVector {
public:
  ShellVector(double x_ = 0.0, double y_ = 0.0) : x(x_), y(y_) {}
  ShellVector operator+(const ShellVector &rhs) const { return ShellVector(x + rhs.x, y + rhs.y); }
  bool operator==(const ShellVector &rhs) const { return x == rhs.x && y == rhs.y; }
  std::string toString() const { return "ShellVector(" + std::to_string(x) + "," + std::to_string(y) + ")"; }
  std::string debugString() const { return "ShellVector[x=" + std::to_string(x) + ",y=" + std::to_string(y) + "]"; }
  std::string repr() const { return debugString(); }
  double x;
  double y;
};

class ShellStat {
public:
  ShellStat() : value_(0.0) {}
  double getValue() const { return value_; }
  void setValue(double v) { value_ = v; }
private:
  double value_;
};

class ShellFileHandle {
public:
  ShellFileHandle() : closed_(false) {}
  void close() { closed_ = true; }
  bool closed() const { return closed_; }
private:
  bool closed_;
};

class ShellCallback {
public:
  virtual ~ShellCallback() {}
  virtual int invoke(int value) { return value; }
  virtual int finalOnly() final { return 0; }
};

class ShellWidget {
public:
  ShellWidget(int id = 0) : id_(id) {}
  int GetId() const { return id_; }
  virtual void Render() {}
  virtual bool Load(const std::string &) { return true; }
private:
  int id_;
};

class ShellContainer {
public:
  template <typename Fn>
  void forEach(Fn) {}
};

enum ShellColour {
  SHELL_COLOUR_RED = 0,
  SHELL_COLOUR_GREEN = 1,
  SHELL_COLOUR_BLUE = 2
};
#endif

// XFeat: fn-decorators
#ifdef SWIGPYTHON
#ifdef XFEAT_FN_DECORATORS
%pythoncode %{
import functools

def logged(fn):
    """Log every call to a wrapped SWIG proxy function."""
    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        print(f"[XFeats] calling {fn.__name__}({args}, {kwargs})")
        result = fn(*args, **kwargs)
        print(f"[XFeats] {fn.__name__} -> {result!r}")
        return result
    return wrapper

# Wrap a generated module-level function
shell_compute = logged(shell_compute)
%}
#endif
#endif

// XFeat: py-properties
#ifdef SWIGPYTHON
#ifdef XFEAT_PY_PROPERTIES
%include <attribute.i>
%attribute(ShellStat, double, value, getValue, setValue);
#endif
#endif

// XFeat: py-builtin
#ifdef SWIGPYTHON
#ifdef XFEAT_PY_BUILTIN
// Enabled with: swig -python -builtin example.i
%feature("python:slot", "tp_repr", functype="reprfunc") ShellVector::repr;
#endif
#endif

// XFeat: py-dunder-repr
#ifdef SWIGPYTHON
#ifdef XFEAT_PY_DUNDER_REPR
%rename(__str__)  ShellVector::toString;
%rename(__repr__) ShellVector::debugString;
%feature("python:slot", "tp_str", functype="reprfunc") ShellVector::toString;
#endif
#endif

// XFeat: py-context-manager
#ifdef SWIGPYTHON
#ifdef XFEAT_PY_CONTEXT_MANAGER
%extend ShellFileHandle {
  %pythoncode %{
  def __enter__(self):
      return self
  def __exit__(self, exc_type, exc_val, exc_tb):
      self.close()
      return False
  %}
}
#endif
#endif

// XFeat: exception-mapping
#ifdef SWIGPYTHON
#ifdef XFEAT_EXCEPTION_MAPPING
%exception {
  try {
    $action
  } catch (const std::out_of_range& e) {
    PyErr_SetString(PyExc_IndexError, e.what());
    SWIG_fail;
  }
}
#endif
#endif

// XFeat: exception-mapping
#ifdef SWIGJAVA
#ifdef XFEAT_EXCEPTION_MAPPING
%typemap(throws, throws="java.lang.IndexOutOfBoundsException")
  std::out_of_range %{
    jclass clazz = jenv->FindClass("java/lang/IndexOutOfBoundsException");
    jenv->ThrowNew(clazz, $1.what());
    return $null;
  %}
#endif
#endif

// XFeat: exception-mapping
#ifdef SWIGCSHARP
#ifdef XFEAT_EXCEPTION_MAPPING
%typemap(throws, canthrow=1) std::out_of_range %{
  SWIG_CSharpSetPendingExceptionArgument(
    SWIG_CSharpArgumentOutOfRangeException, $1.what(), "");
  return $null;
%}
#endif
#endif

// XFeat: exception-mapping
#ifdef SWIGRUBY
#ifdef XFEAT_EXCEPTION_MAPPING
/* No Ruby usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: operator-overloading
#ifdef SWIGPYTHON
#ifdef XFEAT_OPERATOR_OVERLOADING
%rename(__add__) ShellVector::operator+;
%rename(__eq__)  ShellVector::operator==;
#endif
#endif

// XFeat: operator-overloading
#ifdef SWIGCSHARP
#ifdef XFEAT_OPERATOR_OVERLOADING
%rename(Add) ShellVector::operator+;
%csmethodmodifiers ShellVector::Add "public static";
#endif
#endif

// XFeat: operator-overloading
#ifdef SWIGLUA
#ifdef XFEAT_OPERATOR_OVERLOADING
// SWIG maps operator+ to the __add metamethod automatically
%rename(__add) ShellVector::operator+;
#endif
#endif

// XFeat: operator-overloading
#ifdef SWIGRUBY
#ifdef XFEAT_OPERATOR_OVERLOADING
/* No Ruby usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: lua-metatables
#ifdef SWIGLUA
#ifdef XFEAT_LUA_METATABLES
%luacode %{
local mt = getmetatable(ShellVector)
mt.__tostring = function(v) return string.format("(%f, %f)", v.x, v.y) end
%}
#endif
#endif

// XFeat: directors
#ifdef SWIGJAVA
#ifdef XFEAT_DIRECTORS
%feature("director") ShellCallback;
#endif
#endif

// XFeat: directors
#ifdef SWIGPYTHON
#ifdef XFEAT_DIRECTORS
%feature("director") ShellCallback;
%feature("nodirector") ShellCallback::finalOnly;
#endif
#endif

// XFeat: directors
#ifdef SWIGCSHARP
#ifdef XFEAT_DIRECTORS
/* No C# usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: directors
#ifdef SWIGRUBY
#ifdef XFEAT_DIRECTORS
/* No Ruby usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: directors
#ifdef SWIGD
#ifdef XFEAT_DIRECTORS
/* No D usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: smart-pointers
#ifdef SWIGPYTHON
#ifdef XFEAT_SMART_POINTERS
%include <std_shared_ptr.i>
%shared_ptr(ShellWidget)
#endif
#endif

// XFeat: smart-pointers
#ifdef SWIGJAVA
#ifdef XFEAT_SMART_POINTERS
%include <std_shared_ptr.i>
%shared_ptr(ShellWidget)
#endif
#endif

// XFeat: smart-pointers
#ifdef SWIGCSHARP
#ifdef XFEAT_SMART_POINTERS
/* No C# usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: java-pragmas
#ifdef SWIGJAVA
#ifdef XFEAT_JAVA_PRAGMAS
%pragma(java) jniclasscode=%{
  static {
    System.loadLibrary("libshell");
  }
%}
#endif
#endif

// XFeat: java-enums
#ifdef SWIGJAVA
#ifdef XFEAT_JAVA_ENUMS
%include <enums.swg>
%javaconst(1);
%rename(ShellColor) ShellColour;
#endif
#endif

// XFeat: cs-typemaps
#ifdef SWIGCSHARP
#ifdef XFEAT_CS_TYPEMAPS
%typemap(cstype)  bool& "out bool"
%typemap(csin)    bool& "out $csinput"
%csmethodmodifiers ShellWidget::Render "public override";
#endif
#endif

// XFeat: cs-nullable
#ifdef SWIGCSHARP
#ifdef XFEAT_CS_NULLABLE
%typemap(cstype)  double* "double?"
%typemap(csin)    double* "$csinput.HasValue ? $csinput.Value : (double?)null"
#endif
#endif

// XFeat: ruby-mixins
#ifdef SWIGRUBY
#ifdef XFEAT_RUBY_MIXINS
%mixin ShellContainer "Enumerable";
%rename("each") ShellContainer::forEach;
#endif
#endif

// XFeat: ruby-to-s
#ifdef SWIGRUBY
#ifdef XFEAT_RUBY_TO_S
%rename("to_s") ShellVector::toString;
%alias  ShellVector::toString "inspect";
#endif
#endif

// XFeat: go-cgo-directives
#ifdef SWIGGO
#ifdef XFEAT_GO_CGO_DIRECTIVES
%go_import("fmt")
%insert(go_wrapper) %{
func DescribeShellWidget(w ShellWidget) string {
    return fmt.Sprintf("ShellWidget(id=%d)", w.GetId())
}
%}
#endif
#endif

// XFeat: go-error-returns
#ifdef SWIGGO
#ifdef XFEAT_GO_ERROR_RETURNS
%exception ShellWidget::Load %{
  try {
    $action
  } catch (const std::exception& e) {
    _swig_gopanic(e.what());
  }
%}
#endif
#endif

// XFeat: std-container-sugar
#ifdef SWIGPYTHON
#ifdef XFEAT_STD_CONTAINER_SUGAR
%include <std_vector.i>
%template(ShellDoubleVector) std::vector<double>;
// Python: v = ShellDoubleVector([1.0, 2.0]); len(v); v[0]; for x in v: ...
#endif
#endif

// XFeat: std-container-sugar
#ifdef SWIGRUBY
#ifdef XFEAT_STD_CONTAINER_SUGAR
/* No Ruby usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: std-container-sugar
#ifdef SWIGJAVA
#ifdef XFEAT_STD_CONTAINER_SUGAR
/* No Java usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: std-container-sugar
#ifdef SWIGCSHARP
#ifdef XFEAT_STD_CONTAINER_SUGAR
/* No C# usage snippet present in bindings/XFeats.yaml. */
#endif
#endif

// XFeat: php-typemaps
#ifdef SWIGPHP
#ifdef XFEAT_PHP_TYPEMAPS
%typemap(in) std::vector<int>* (std::vector<int> tmp) %{
  // convert incoming PHP array to std::vector<int>
%}
#endif
#endif

// XFeat: perl-blessed-refs
#ifdef SWIGPERL
#ifdef XFEAT_PERL_BLESSED_REFS
%perlcode %{
push @ShellWidget::ISA, 'Exporter';
%}
#endif
#endif

// XFeat: r-s4-classes
#ifdef SWIGR
#ifdef XFEAT_R_S4_CLASSES
%feature("S4") ShellWidget;
#endif
#endif

// XFeat: d-templates
#ifdef SWIGD
#ifdef XFEAT_D_TEMPLATES
%include <std_vector.i>
%template(ShellIntVector) std::vector<int>;
#endif
#endif

// XFeat: node-napi-async
#ifdef SWIGJAVASCRIPT
#ifdef XFEAT_NODE_NAPI_ASYNC
%insert(js_wrapper) %{
exports.shell_computeAsync = function(...args) {
  return new Promise((resolve) => resolve(exports.shell_compute(...args)));
};
%}
#endif
#endif
