"""Shared assembly of the mlxcx command queue under the device model."""

from c_test import (TESTDIR, CTestFailure, function, mlxcx_types,
                    optional_function, region,
                    run_c, scenarios, source_parser)


WRAPPERS = ("mlxcx_cmd_enable_hca", "mlxcx_cmd_query_pages",
            "mlxcx_cmd_give_pages", "mlxcx_cmd_return_pages",
            "mlxcx_cmd_query_hca_cap", "mlxcx_cmd_alloc_uar")


def command_source(srcdir):
    cmd = srcdir / "mlxcx_cmd.c"
    parts = [
        region(cmd, "mlxcx_cmd_response_string", "mlxcx_cmd_opcode_string"),
        preamble(cmd),
        region(cmd, "mlxcx_cmd_queue_fini", "mlxcx_cmd_evaluate"),
    ]
    parts += [function(cmd, name) for name in WRAPPERS]
    return "\n".join(parts)


def preamble(path):
    """The tunables and macros between the includes and the first function."""
    text = path.read_text(encoding="utf-8")
    start = text.index("#include <sys/sysmacros.h>\n") + len(
        "#include <sys/sysmacros.h>\n")
    end = text.index("static const char *\nmlxcx_cmd_response_string")
    line = text.count("\n", 0, start) + 1
    return f'#line {line} "{path}"\n{text[start:end]}\n'


def run(doc, test, names, extra=None):
    args = source_parser(doc).parse_args()
    headers = {
        "mlxcx_types.h": mlxcx_types(args.source_dir),
        "mlxcx_cmd_body.h": command_source(args.source_dir),
        "mlxcx_extra_body.h": extra(args.source_dir) if extra else "",
    }
    try:
        run_c(TESTDIR / test, headers, args.source_dir,
              cases=scenarios(args, names))
    except CTestFailure as error:
        raise SystemExit(f"FAIL {test}: {error}") from None


__all__ = ("run", "optional_function")
