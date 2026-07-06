"""The itm transport CLI callback: it builds a TraceDB from --elf and returns an
ITM_Transport without starting it, so it can be driven with a fixture ELF and no
serial port."""

import typer
from typer.testing import CliRunner

from tilogger_itm_transport.itm_transport import transport_factory_cli


def _app():
    # A no-op callback keeps this a command group (Typer otherwise promotes a
    # lone command to the root and swallows the 'itm' name as a positional).
    app = typer.Typer()

    @app.callback()
    def _root():
        pass

    transport_factory_cli(app)
    return app


def test_itm_cli_builds_transport_with_elf(log_elf):
    result = CliRunner().invoke(_app(), ["itm", "--elf", str(log_elf), "/dev/null", "12000000"])
    assert result.exit_code == 0, result.output


def test_itm_cli_without_elf_errors():
    result = CliRunner().invoke(_app(), ["itm", "/dev/null", "12000000"])
    assert result.exit_code != 0  # symbols are required


def test_itm_cli_help_lists_late_attach():
    result = CliRunner().invoke(_app(), ["itm", "--help"])
    assert result.exit_code == 0
    assert "late-attach" in result.output
