# -*- coding: utf-8 -*-
"""
番茄小说下载器 (MCP 版)
通过 MCP 协议调用 @fysh925/mcp-server-fanqie 完成搜索/目录/内容下载
"""
import os, sys, json, time, subprocess, threading
from queue import Queue
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

MCP_BIN = "C:/Users/18301/AppData/Roaming/npm/node_modules/@fysh925/mcp-server-fanqie/build/index.js"
SAVE_DIR = os.path.join(os.path.expanduser("~"), "Desktop", "番茄小说下载")
CONCURRENCY = 16
RETRY = 3          # 单章最大重试次数
RETRY_DELAY = 1.0  # 重试间隔(秒)
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
NL = chr(10)
BACKSLASH = chr(92)
ILLEGAL = BACKSLASH + '/:*?"<>|'


def safe_name(name):
    for c in ILLEGAL:
        name = name.replace(c, "_")
    return name


class McpClient:
    """单个 MCP server 进程（node 子进程 + JSON-RPC）"""
    def __init__(self):
        self.p = subprocess.Popen(
            ["node", MCP_BIN],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            creationflags=CREATE_NO_WINDOW,
        )
        self._id = 1
        self._send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "dl", "version": "1.0"}}})
        self._read()
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

    def _send(self, obj):
        self.p.stdin.write((json.dumps(obj) + NL).encode("utf-8"))
        self.p.stdin.flush()

    def _read(self):
        return self.p.stdout.readline().decode("utf-8", "ignore").strip()

    def call(self, name, args):
        self._id += 1
        rid = self._id
        self._send({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
                    "params": {"name": name, "arguments": args}})
        for _ in range(5000):
            line = self._read()
            if not line:
                return ""
            try:
                r = json.loads(line)
            except Exception:
                continue
            if r.get("id") == rid:
                c = r.get("result", {}).get("content", [])
                return c[0].get("text", "") if c else ""
        return ""

    def close(self):
        try:
            self.p.terminate()
        except Exception:
            pass


class McpPool:
    """MCP 客户端池（线程安全）"""
    def __init__(self, size):
        self.q = Queue()
        for _ in range(size):
            self.q.put(McpClient())

    def call(self, name, args):
        c = self.q.get()
        try:
            return c.call(name, args)
        finally:
            self.q.put(c)

    def close(self):
        while not self.q.empty():
            try:
                self.q.get_nowait().close()
            except Exception:
                pass


class FanqieMcpDownloader:
    def __init__(self, concurrency=CONCURRENCY):
        self.concurrency = concurrency
        self.pool = McpPool(concurrency)
        os.makedirs(SAVE_DIR, exist_ok=True)

    def search(self, keyword):
        txt = self.pool.call("search_books", {"key": keyword, "tab_type": 3, "offset": 0})
        try:
            d = json.loads(txt)
        except Exception:
            return []
        books, seen = [], set()
        results = d.get("results", {})
        for tab in results.get("search_tabs", []):
            bd = tab.get("data")
            if not isinstance(bd, list):
                continue
            for cell in bd:
                for b in (cell.get("book_data") or []):
                    bid = str(b.get("book_id", ""))
                    if bid and bid not in seen:
                        seen.add(bid)
                        books.append({
                            "book_id": bid,
                            "book_name": b.get("book_name", "未知"),
                            "author": b.get("author", "未知"),
                            "score": str(b.get("score", "")),
                            "abstract": (b.get("abstract") or "").replace(NL, " ")[:70],
                        })
        return books

    def get_directory(self, book_id):
        txt = self.pool.call("get_simple_directory", {"book_id": book_id})
        try:
            d = json.loads(txt)
        except Exception:
            return []
        lists = d.get("simple_directory", {}).get("lists", [])
        return [(str(x.get("item_id", "")), x.get("title", "未命名")) for x in lists]

    def get_chapter(self, item_id):
        """单次获取章节内容（重试由 download 的轮次机制负责）。返回 (content, error)。"""
        try:
            txt = self.pool.call("get_chapter_content", {"item_id": item_id})
            d = json.loads(txt)
            if d.get("error"):
                return "", str(d.get("message", "unknown"))
            content = d.get("content", {}).get("content", "")
            if content and content.strip():
                return content, ""
            return "", "empty content"
        except Exception as e:
            return "", str(e)

    def download(self, book_id, book_name, max_chapters=None):
        print("")
        print("📖 《" + book_name + "》")
        chapters = self.get_directory(book_id)
        if not chapters:
            print("❌ 未获取到目录")
            return
        print("   共 " + str(len(chapters)) + " 章")
        if max_chapters:
            chapters = chapters[:max_chapters]

        book_dir = os.path.join(SAVE_DIR, safe_name(book_name))
        os.makedirs(book_dir, exist_ok=True)

        done = set()
        for f in os.listdir(book_dir):
            m = f.split("-")[0]
            if m.isdigit():
                done.add(int(m))

        todo = [(idx, iid, title) for idx, (iid, title) in enumerate(chapters, 1) if idx not in done]
        if not todo:
            print("✅ 已全部下载")
            self._merge(book_dir, book_name, chapters)
            return

        print("   待下载 " + str(len(todo)) + " 章，并发 " + str(self.concurrency)
              + "，重试 " + str(RETRY) + " 次 ...")
        total = len(chapters)
        ok = 0
        failed = []   # [(idx, iid, title)]
        t0 = time.time()
        lock = threading.Lock()

        def save_chapter(idx, title, content):
            fp = os.path.join(book_dir, ("%04d-" % idx) + safe_name(title) + ".txt")
            with open(fp, "w", encoding="utf-8") as f:
                f.write(content.strip())

        def job(item):
            idx, iid, title = item
            content, err = self.get_chapter(iid)
            return idx, iid, title, content, err

        # 轮次：首轮 + 失败重试轮（最多 RETRY 轮额外重试）
        batch = todo
        for round_no in range(1, RETRY + 2):
            if not batch:
                break
            if round_no > 1:
                print(NL + "   🔄 第 " + str(round_no - 1) + " 轮重试，剩余 "
                      + str(len(batch)) + " 章 ...")
                time.sleep(RETRY_DELAY * (round_no - 1))

            next_batch = []
            with ThreadPoolExecutor(max_workers=self.concurrency) as ex:
                futs = {ex.submit(job, t): t for t in batch}
                for fut in as_completed(futs):
                    try:
                        idx, iid, title, content, err = fut.result()
                    except Exception as e:
                        idx, iid, title, content, err = futs[fut][0], futs[fut][1], futs[fut][2], "", str(e)
                    if content:
                        save_chapter(idx, title, content)
                        with lock:
                            ok += 1
                        print("   [" + str(idx) + "/" + str(total) + "] ✅ "
                              + title + " (" + str(len(content)) + "字)")
                    else:
                        next_batch.append((idx, iid, title))
                        if round_no == 1:
                            print("   [" + str(idx) + "/" + str(total) + "] ⚠ 失败: "
                                  + title + "  (" + err + ")")
            batch = next_batch

        failed = batch
        elapsed = time.time() - t0
        print("")
        print("🎉 完成！成功 " + str(ok) + " 章，耗时 " + ("%.1f" % elapsed) + "s")
        if failed:
            print("❌ 最终失败 " + str(len(failed)) + " 章:")
            for idx, iid, title in failed[:20]:
                print("   [" + str(idx) + "] " + title + " (item_id=" + iid + ")")
            if len(failed) > 20:
                print("   ... 其余 " + str(len(failed) - 20) + " 章")
        self._merge(book_dir, book_name, chapters)

    def _merge(self, book_dir, book_name, chapters):
        merged = os.path.join(SAVE_DIR, book_name + "_全本.txt")
        n = 0
        sep = "=" * 50
        with open(merged, "w", encoding="utf-8") as out:
            for idx, (_, title) in enumerate(chapters, 1):
                fp = os.path.join(book_dir, ("%04d-" % idx) + safe_name(title) + ".txt")
                if os.path.exists(fp):
                    with open(fp, encoding="utf-8") as f:
                        out.write(NL + NL + sep + NL + title + NL + sep + NL + NL)
                        out.write(f.read())
                    n += 1
        print("📄 合并版 (" + str(n) + "章): " + merged)

    def close(self):
        self.pool.close()


def parse_selection(text, max_n):
    """解析多选输入，返回去重排序后的序号列表(1-based)。
    支持: '1'、'1 3 5'、'1,3,5'、'1-3'、'1 2-4 6' 等混合写法。"""
    result = set()
    # 统一分隔符：逗号、顿号、空格 -> 空格
    for c in [",", "，", "、", ";", "；"]:
        text = text.replace(c, " ")
    for token in text.split():
        if "-" in token:
            parts = token.split("-")
            if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
                a, b = int(parts[0]), int(parts[1])
                if a > b:
                    a, b = b, a
                for x in range(a, b + 1):
                    if 1 <= x <= max_n:
                        result.add(x)
        elif token.isdigit():
            x = int(token)
            if 1 <= x <= max_n:
                result.add(x)
    return sorted(result)


def main():
    print("=" * 55)
    print("     番茄小说下载器 v5.1 (MCP)")
    print("=" * 55)

    dl = FanqieMcpDownloader()
    try:
        while True:
            print(NL + "-" * 55)
            kw = input(NL + "请输入书名 (输入 q 退出): ").strip()
            if not kw or kw.lower() == "q":
                print(NL + "再见！")
                break

            print(NL + "🔍 搜索「" + kw + "」...")
            books = dl.search(kw)
            if not books:
                print("❌ 未找到相关书籍，请换个关键词试试")
                continue

            print(NL + "找到 " + str(len(books)) + " 本:" + NL)
            for i, b in enumerate(books, 1):
                print("  " + str(i) + ". 《" + b["book_name"] + "》 - " + b["author"] + " [评分" + b["score"] + "]")
                if b["abstract"]:
                    print("      " + b["abstract"] + "...")

            sel = input(NL + "请选择 (如 1 / 1 3 5 / 1-3 / 1,2,4，回车取消): ").strip()
            if not sel:
                continue
            choices = parse_selection(sel, len(books))
            if not choices:
                print("⚠ 输入无效或超出范围，请重新选择")
                continue

            print(NL + "已选择 " + str(len(choices)) + " 本:")
            for ch in choices:
                print("   " + str(ch) + ". 《" + books[ch - 1]["book_name"] + "》")

            lim = input("每本下载章节数 (回车=全部): ").strip()
            try:
                mx = int(lim) if lim else None
            except ValueError:
                mx = None

            for n, ch in enumerate(choices, 1):
                book = books[ch - 1]
                print(NL + ">>> 第 " + str(n) + "/" + str(len(choices)) + " 本 <<<")
                try:
                    dl.download(book["book_id"], book["book_name"], max_chapters=mx)
                except Exception as e:
                    print("❌ 《" + book["book_name"] + "》下载出错: " + str(e))
    except KeyboardInterrupt:
        print(NL + NL + "已中断，再见！")
    finally:
        dl.close()


if __name__ == "__main__":
    main()
