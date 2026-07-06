"""Shared fixtures for the core decode tests.

log_elf builds a tiny ELF that carries the LogSinkBuf .log_data/.log_ptr
sections plus DWARF, using the host C toolchain. The decode tooling is
architecture-neutral - it reads sections, symbols and DWARF and never looks at
the ELF machine type - so a host x86/arm ELF drives exactly the same
TraceDB.parse_elf and tilogger.dwarf paths a real CC27xx/CC23xx .out would,
with no device or cross-compiler needed. Tests skip where no cc is available.
"""

import shutil
import subprocess

import pytest

# Two log records: a formatted-text site and a buffer site. \036 is the 0x1e
# record separator TraceDB splits on (octal so the assembler does not eat the
# following hex digits).
_LOGSEC_S = r"""
	.section .log_data,"a",@progbits
LogSymbol_x:
	.asciz "LOG_OPCODE_FORMATED_TEXT\036file.c\03642\036Log_DEBUG\036LogMod_App\036x=%d\0361"
	.size LogSymbol_x, .-LogSymbol_x
LogSymbol_buf:
	.asciz "LOG_OPCODE_BUFFER\036buf.c\03699\036Log_INFO\036LogMod_App\036dump \0360"
	.size LogSymbol_buf, .-LogSymbol_buf
	.section .log_ptr,"a",@progbits
Ptr_LogSymbol_x:
	.long LogSymbol_x
	.size Ptr_LogSymbol_x, 4
Ptr_LogSymbol_buf:
	.long LogSymbol_buf
	.size Ptr_LogSymbol_buf, 4
"""

_FUNCS_C = """
int demo_add(int a, int b) { return a + b; }
int demo_mul(int a, int b) { return a * b; }
int main(void) { return demo_add(demo_mul(2, 3), 1); }
"""


@pytest.fixture(scope="session")
def log_elf(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C toolchain to build the log ELF fixture")
    d = tmp_path_factory.mktemp("logelf")
    (d / "funcs.c").write_text(_FUNCS_C)
    (d / "logsec.s").write_text(_LOGSEC_S)
    elf = d / "fixture.elf"
    try:
        # DWARF (-g) directives cannot reference the custom log sections, so the
        # log sections are assembled without debug info and linked in.
        subprocess.run([cc, "-c", "-g", str(d / "funcs.c"), "-o", str(d / "funcs.o")],
                       check=True, capture_output=True)
        subprocess.run([cc, "-c", str(d / "logsec.s"), "-o", str(d / "logsec.o")],
                       check=True, capture_output=True)
        subprocess.run([cc, "-g", "-no-pie", str(d / "logsec.o"), str(d / "funcs.o"), "-o", str(elf)],
                       check=True, capture_output=True)
    except (subprocess.CalledProcessError, OSError) as exc:
        pytest.skip("log ELF fixture build failed: %s" % exc)
    return elf
