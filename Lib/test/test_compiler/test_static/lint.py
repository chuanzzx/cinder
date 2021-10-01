from .common import StaticTestBase


class LintTests(StaticTestBase):
    def test_lint_ifexp(self) -> None:
        codestr = """
        from __static__ import int64

        def f(x: bool) -> int64:
            return "foo" if x else 0
        """

        errors = self.lint(codestr)
        errors.check(
            errors.match(
                "invalid union type Union[Exact[str], Literal[0]]",
                at='"foo"',
            ),
        )
