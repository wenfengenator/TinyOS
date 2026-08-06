# SPDX-License-Identifier: GPL-2.0-only

import taudio

print("audio ready:", taudio.ready())
print("diagnostics:", taudio.info())
for frequency in (262, 330, 392, 523):
    taudio.tone(frequency, 140, 70)