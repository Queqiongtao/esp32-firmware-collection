# SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: CC0-1.0
import re

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize('target', ['esp32p4'], indirect=['target'])
def test_JC1060P470(dut: Dut) -> None:
    dut.expect_exact('Display LVGL integrated demo UI (static patch font)', timeout=30)
    dut.expect(re.compile(r'startup_summary enabled=\d+ ready=\d+ failed=\d+ first_error=\S+'),
               timeout=120)
