from compiler.static.compiler import Compiler
from compiler.static.types import TypeEnvironment
from compiler.strict.compiler import Compiler as StrictCompiler

from .common import StaticTestBase, StaticTestsStrictModuleLoader


class ModuleTests(StaticTestBase):
    def decl_visit(self, **modules: str) -> Compiler:
        compiler = self.compiler(**modules)
        for name in modules.keys():
            compiler.import_module(name, optimize=0)
        return compiler

    def test_import_name(self) -> None:
        acode = """
            def foo(x: int) -> int:
               return x
        """
        bcode = """
            import a
        """
        compiler = self.decl_visit(a=acode, b=bcode)

        self.assertIn("b", compiler.modules)
        self.assertIn("a", compiler.modules["b"].children)
        self.assertEqual(
            compiler.modules["b"].children["a"].klass, compiler.type_env.module
        )
        self.assertEqual(compiler.modules["b"].children["a"].module_name, "a")

    def test_import_name_as(self) -> None:
        acode = """
            def foo(x: int) -> int:
               return x
        """
        bcode = """
            import a as foo
        """
        compiler = self.decl_visit(a=acode, b=bcode)

        self.assertIn("foo", compiler.modules["b"].children)
        self.assertEqual(
            compiler.modules["b"].children["foo"].klass, compiler.type_env.module
        )
        self.assertEqual(compiler.modules["b"].children["foo"].module_name, "a")

    def test_import_module_within_directory(self) -> None:
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            import a.b
        """
        compiler = self.decl_visit(**{"a.b": abcode, "c": ccode})

        self.assertIn("a", compiler.modules["c"].children)
        self.assertEqual(
            compiler.modules["c"].children["a"].klass, compiler.type_env.module
        )
        self.assertEqual(compiler.modules["c"].children["a"].module_name, "a")

    def test_import_module_within_directory_as(self) -> None:
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            import a.b as m
        """
        compiler = self.decl_visit(**{"a.b": abcode, "c": ccode})

        self.assertIn("m", compiler.modules["c"].children)
        self.assertEqual(
            compiler.modules["c"].children["m"].klass, compiler.type_env.module
        )
        self.assertEqual(compiler.modules["c"].children["m"].module_name, "a.b")

    def test_import_module_within_directory_from(self) -> None:
        acode = """
            pass
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        self.assertIn("b", compiler.modules["c"].children)
        self.assertEqual(
            compiler.modules["c"].children["b"].klass, compiler.type_env.module
        )
        self.assertEqual(compiler.modules["c"].children["b"].module_name, "a.b")

    def test_import_module_within_directory_from_as(self) -> None:
        acode = """
            pass
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b as zoidberg
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        self.assertIn("zoidberg", compiler.modules["c"].children)
        self.assertEqual(
            compiler.modules["c"].children["zoidberg"].klass, compiler.type_env.module
        )
        self.assertEqual(compiler.modules["c"].children["zoidberg"].module_name, "a.b")

    def test_import_module_within_directory_from_where_value_exists(self) -> None:
        acode = """
            b: int = 1
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        self.assertIn("b", compiler.modules["c"].children)
        self.assertEqual(
            compiler.modules["c"].children["b"].klass, compiler.type_env.int
        )

    def test_import_module_within_directory_from_where_untyped_value_exists(
        self,
    ) -> None:
        acode = """
            b = 1
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        self.assertIn("b", compiler.modules["c"].children)
        # Note that since the declaration visitor doesn't distinguish between
        # untyped values and missing ones, we resolve to the module type where that might
        # not have been the intention.
        self.assertEqual(
            compiler.modules["c"].children["b"].klass, compiler.type_env.module
        )

    def test_import_chaining(self) -> None:
        acode = """
            def foo(x: int) -> int: return x
        """
        bcode = """
            import a
        """
        ccode = """
            import b

            def f():
               return b.a.foo(1)
        """
        compiler = self.compiler(a=acode, b=bcode, c=ccode)
        f = self.find_code(compiler.compile_module("c"), "f")
        self.assertInBytecode(f, "INVOKE_FUNCTION", (("a", "foo"), 1))

    def test_module_special_name_access(self) -> None:
        acode = """
            def foo(x: int) -> int: return x
        """
        bcode = """
            import a
        """
        ccode = """
            import b

            def f():
               reveal_type(b.a)
        """
        compiler = self.compiler(a=acode, b=bcode, c=ccode)
        compiler.revealed_type("c", r"Exact[types.ModuleType]")

    def test_repeated_import(self) -> None:
        codestr = """
            def foo():
                pass
        """
        compiler = self.get_strict_compiler()

        compiler.load_compiled_module_from_source(
            self.clean_code(codestr), "mod.py", "mod", 1
        )
        compiler.load_compiled_module_from_source(
            self.clean_code(codestr), "mod.py", "mod", 1
        )

    def test_recursive_imports(self) -> None:
        acode = """
            from typing import TYPE_CHECKING
            if TYPE_CHECKING:
                from b import X

            class C:
                pass
        """
        bcode = """
            from a import C
            from typing import Optional
            from __static__ import cast

            class X:
                def f(self, v) -> Optional[C]:
                    if isinstance(v, C):
                        return cast(C, v)
        """
        compiler = self.decl_visit(**{"a": acode, "b": bcode})
        compiler.compile_module("b")
