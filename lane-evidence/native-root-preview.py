import gdb, json
from collections import Counter
counts = Counter()
seen = set()
def out(s): print(s, flush=True)
gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set breakpoint pending on')
gdb.execute('set disable-randomization on')
gdb.execute('handle SIGSEGV nostop noprint pass')
gdb.execute('handle SIGABRT stop print pass')
class Root(gdb.Breakpoint):
    def stop(self):
        try:
            addr = int(gdb.parse_and_eval('&field'))
            word = int.from_bytes(gdb.selected_inferior().read_memory(addr, 8), 'little')
            payload = word & ((1 << 48) - 1)
            reject = payload != 0 and word & (0xff << 48) == 0
            kind = 'reject' if reject else ('null' if payload == 0 else 'colored')
            counts[(self.location, kind)] += 1
            key = (addr, word)
            if reject and key not in seen:
                seen.add(key)
                out('PREVIEW_REJECT slot=%#x word=%#x entry=%s' % (addr, word, self.location))
                out(gdb.execute('info symbol %#x' % addr, to_string=True).strip())
                try:
                    hdr = int.from_bytes(gdb.selected_inferior().read_memory(payload, 8), 'little')
                    name = gdb.parse_and_eval('((MapleRuntime::TypeInfo*)%#x)->typeInfoName' % (hdr & ((1<<48)-1)))
                    out('PREVIEW_TARGET header=%#x type=%s' % (hdr, name))
                except Exception as e: out('PREVIEW_TARGET_UNAVAILABLE %s' % e)
                out(gdb.execute('bt 18', to_string=True))
        except Exception as e:
            counts[('error', str(e))] += 1
        return False
Root('MapleRuntime::Barrier::ReadStaticRef')
Root('MapleRuntime::WCollector::EnumRefFieldRoot')
gdb.execute('run')
out('PREVIEW_STOP')
out(gdb.execute('info program', to_string=True))
try: out(gdb.execute('info proc mappings', to_string=True))
except Exception: pass
for (entry, kind), count in counts.items(): out('PREVIEW_COUNT entry=%s kind=%s n=%d' % (entry,kind,count))
out('PREVIEW_UNIQUE_REJECTS=%d' % len(seen))
if gdb.selected_inferior().pid: gdb.execute('kill')
