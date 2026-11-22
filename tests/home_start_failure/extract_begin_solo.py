"""Extract the actual UI callback; never execute the full GUI in this fixture."""
import sys
from pathlib import Path
text=Path(sys.argv[1]).read_text(encoding='utf-8')
start='    auto begin_solo = [&](rt::StartRaceRequest request) {'
end='    bool start_solo_ok = !start_solo;'
if text.count(start)!=1 or text.count(end)!=1:
    raise SystemExit('begin_solo markers changed; update test extraction explicitly')
body=text[text.index(start):text.index(end)]
if not body.rstrip().endswith('};'):
    raise SystemExit('unexpected callback boundary')
Path(sys.argv[2]).write_text(body,encoding='utf-8')
