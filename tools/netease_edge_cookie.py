#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从本机 Microsoft Edge 提取网易云音乐 cookie，供 ebook_reader 导入测试。

只使用 Python 标准库：
  * Windows DPAPI (CryptUnprotectData) 通过 ctypes 调用；
  * AES-256-GCM 为纯 Python 实现（pip 源不可用时的兜底）。

Edge / Chrome 的 Cookies SQLite 库在浏览器运行时被独占锁定，
因此用 CreateFileW(FILE_SHARE_READ|WRITE|DELETE) 读出字节流到临时文件再查询，
不修改浏览器 profile 的任何文件。

用法：
  python tools/netease_edge_cookie.py --check
  python tools/netease_edge_cookie.py --selftest
  # Edge 独占锁定 Cookies 库时：先用 --save-copy 导出一次，之后走 --cookies-db
  python tools/netease_edge_cookie.py --save-copy /tmp/edge_cookies.db --check
  python tools/netease_edge_cookie.py --cookies-db /tmp/edge_cookies.db \
      --api http://127.0.0.1:8686
  python tools/netease_edge_cookie.py --cookies-db /tmp/edge_cookies.db \
      --domain 163.com --output cookie.txt
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
import urllib.request
from ctypes import wintypes

# ---------------------------------------------------------------------------
# GF(2^8) / AES
# ---------------------------------------------------------------------------


def _gmul(a: int, b: int) -> int:
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p


def _ginv(a: int) -> int:
    if a == 0:
        return 0
    for b in range(1, 256):
        if _gmul(a, b) == 1:
            return b
    return 0


def _rotl8(x: int, n: int) -> int:
    return ((x << n) | (x >> (8 - n))) & 0xFF


def _make_sbox() -> list[int]:
    box = []
    for a in range(256):
        inv = _ginv(a)
        s = inv
        for n in range(1, 5):
            s ^= _rotl8(inv, n)
        box.append(s ^ 0x63)
    return box


SBOX = _make_sbox()


def _expand_key(key: bytes) -> list[list[int]]:
    assert len(key) == 32
    nk, nr = 8, 14
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = w[i - 1][:]
        if i % nk == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _gmul(rcon, 2)
        elif i % nk == 4:
            t = [SBOX[b] for b in t]
        w.append([w[i - nk][j] ^ t[j] for j in range(4)])
    return [w[4 * r] + w[4 * r + 1] + w[4 * r + 2] + w[4 * r + 3]
            for r in range(nr + 1)]


def _aes_encrypt_block(block: bytes, rk: list[list[int]]) -> bytes:
    # state 按列优先展平：state[4*c + r]，c=列 r=行
    s = [block[i] ^ rk[0][i] for i in range(16)]

    def sub_bytes() -> None:
        for i in range(16):
            s[i] = SBOX[s[i]]

    def shift_rows() -> None:
        old = s[:]
        for c in range(4):
            for r in range(4):
                s[4 * c + r] = old[4 * ((c + r) % 4) + r]

    def mix_columns() -> None:
        for c in range(4):
            a0, a1, a2, a3 = s[4 * c:4 * c + 4]
            s[4 * c + 0] = _gmul(a0, 2) ^ _gmul(a1, 3) ^ a2 ^ a3
            s[4 * c + 1] = a0 ^ _gmul(a1, 2) ^ _gmul(a2, 3) ^ a3
            s[4 * c + 2] = a0 ^ a1 ^ _gmul(a2, 2) ^ _gmul(a3, 3)
            s[4 * c + 3] = _gmul(a0, 3) ^ a1 ^ a2 ^ _gmul(a3, 2)

    for r in range(1, nr := 14):
        sub_bytes()
        shift_rows()
        mix_columns()
        s = [s[i] ^ rk[r][i] for i in range(16)]
    sub_bytes()
    shift_rows()
    return bytes(s[i] ^ rk[nr][i] for i in range(16))


# ---------------------------------------------------------------------------
# GCM
# ---------------------------------------------------------------------------

_GCM_R = 0xE1000000000000000000000000000000


def _gmul128(x: int, h: int) -> int:
    z, v = 0, h
    for i in range(128):
        if (x >> (127 - i)) & 1:
            z ^= v
        v = (v >> 1) ^ _GCM_R if v & 1 else v >> 1
    return z


def _ghash(h: int, aad: bytes, ct: bytes) -> bytes:
    data = aad + b'\0' * (-len(aad) % 16) + ct + b'\0' * (-len(ct) % 16)
    data += (len(aad) * 8).to_bytes(8, 'big') + (len(ct) * 8).to_bytes(8, 'big')
    y = 0
    for i in range(0, len(data), 16):
        y = _gmul128(y ^ int.from_bytes(data[i:i + 16], 'big'), h)
    return y.to_bytes(16, 'big')


def aes_gcm_decrypt(key: bytes, nonce: bytes, blob: bytes, aad: bytes = b'') -> bytes:
    """blob = ciphertext || 16-byte tag。tag 校验失败抛 ValueError。"""
    if len(blob) < 16:
        raise ValueError('ciphertext too short')
    ct, tag = blob[:-16], blob[-16:]
    rk = _expand_key(key)
    h = int.from_bytes(_aes_encrypt_block(b'\0' * 16, rk), 'big')
    j0 = nonce + b'\x00\x00\x00\x01' if len(nonce) == 12 else None
    if j0 is None:
        raise ValueError('only 12-byte nonces are supported')
    stream = bytearray()
    counter = int.from_bytes(j0[12:], 'big')
    for i in range((len(ct) + 15) // 16):
        stream += _aes_encrypt_block(j0[:12] + ((counter + 1 + i) & 0xFFFFFFFF).to_bytes(4, 'big'), rk)
    plain = bytes(a ^ b for a, b in zip(ct, stream))
    want = bytes(a ^ b for a, b in zip(_aes_encrypt_block(j0, rk), _ghash(h, aad, ct)))
    if not _ct_eq(tag, want):
        raise ValueError('GCM authentication tag mismatch')
    return plain


def _ct_eq(a: bytes, b: bytes) -> bool:
    if len(a) != len(b):
        return False
    d = 0
    for x, y in zip(a, b):
        d |= x ^ y
    return d == 0


# ---------------------------------------------------------------------------
# Windows DPAPI
# ---------------------------------------------------------------------------


class _DATA_BLOB(ctypes.Structure):
    _fields_ = [('cbData', wintypes.DWORD),
                ('pbData', ctypes.POINTER(ctypes.c_byte))]


def dpapi_decrypt(data: bytes) -> bytes:
    if os.name != 'nt':
        raise RuntimeError('DPAPI is Windows-only')
    src_buf = (ctypes.c_byte * len(data)).from_buffer_copy(data)
    src = _DATA_BLOB(len(data), ctypes.cast(src_buf, ctypes.POINTER(ctypes.c_byte)))
    dst = _DATA_BLOB()
    crypt32 = ctypes.windll.crypt32
    crypt32.CryptUnprotectData.argtypes = [
        ctypes.POINTER(_DATA_BLOB), wintypes.LPCWSTR, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(_DATA_BLOB)]
    crypt32.CryptUnprotectData.restype = wintypes.BOOL
    ok = crypt32.CryptUnprotectData(ctypes.byref(src), None, None, None, None,
                                    0x01,  # CRYPTPROTECT_UI_FORBIDDEN
                                    ctypes.byref(dst))
    if not ok:
        raise OSError('CryptUnprotectData failed, winerror=%s' % ctypes.get_last_error())
    try:
        out = (ctypes.c_ubyte * dst.cbData).from_address(
            ctypes.cast(dst.pbData, ctypes.c_void_p).value)
        return bytes(out)
    finally:
        ctypes.windll.kernel32.LocalFree(ctypes.cast(dst.pbData, ctypes.c_void_p))


def dpapi_encrypt(data: bytes) -> bytes:
    if os.name != 'nt':
        raise RuntimeError('DPAPI is Windows-only')
    src_buf = (ctypes.c_byte * len(data)).from_buffer_copy(data)
    src = _DATA_BLOB(len(data), ctypes.cast(src_buf, ctypes.POINTER(ctypes.c_byte)))
    dst = _DATA_BLOB()
    crypt32 = ctypes.windll.crypt32
    crypt32.CryptProtectData.argtypes = [
        ctypes.POINTER(_DATA_BLOB), wintypes.LPCWSTR, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(_DATA_BLOB)]
    crypt32.CryptProtectData.restype = wintypes.BOOL
    ok = crypt32.CryptProtectData(ctypes.byref(src), None, None, None, None,
                                  0x01, ctypes.byref(dst))
    if not ok:
        raise OSError('CryptProtectData failed, winerror=%s' % ctypes.get_last_error())
    try:
        out = (ctypes.c_ubyte * dst.cbData).from_address(
            ctypes.cast(dst.pbData, ctypes.c_void_p).value)
        return bytes(out)
    finally:
        ctypes.windll.kernel32.LocalFree(ctypes.cast(dst.pbData, ctypes.c_void_p))


# ---------------------------------------------------------------------------
# Edge cookie 库
# ---------------------------------------------------------------------------

DEFAULT_USER_DATA = os.path.join(
    os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'Edge', 'User Data')

CORE_KEYS = ['MUSIC_A', 'MUSIC_U', 'MUSIC_A_T', 'MUSIC_R_T', '__csrf', 'NMTID']


def _read_locked_file(path: str) -> bytes:
    """用 FILE_SHARE_READ|WRITE|DELETE 打开被 Edge 独占的 Cookies 库。"""
    if os.name != 'nt':
        with open(path, 'rb') as f:
            return f.read()
    CreateFileW = ctypes.WinDLL('kernel32', use_last_error=True).CreateFileW
    CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                            wintypes.HANDLE]
    CreateFileW.restype = wintypes.HANDLE
    handle = CreateFileW(path, 0x80000000, 0x1 | 0x2 | 0x4, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value or handle is None:
        raise OSError('CreateFileW failed (winerror=%s)。Edge 可能独占锁定了 Cookies 库，'
                      '可先用 --cookies-db 指向导出的副本。' % ctypes.get_last_error())
    import msvcrt
    fd = msvcrt.open_osfhandle(handle, os.O_RDONLY)
    with os.fdopen(fd, 'rb') as f:
        return f.read()


def _aes_key(user_data: str) -> bytes:
    with open(os.path.join(user_data, 'Local State'), 'r', encoding='utf-8') as f:
        state = json.load(f)
    raw = state.get('os_crypt', {}).get('encrypted_key')
    if not raw:
        raise RuntimeError('Local State 里没有 os_crypt.encrypted_key')
    blob = base64.b64decode(raw)
    if blob.startswith(b'DPAPI'):
        blob = blob[5:]
    return dpapi_decrypt(blob)


def _decrypt_value(value: bytes, key: bytes, host_key: str) -> tuple[str, str]:
    """返回 (明文, 加密版本)。无法解密时返回 ('', 版本标记)。"""
    if not value:
        return '', 'empty'
    if value.startswith(b'v10'):
        try:
            plain = aes_gcm_decrypt(key, value[3:15], value[15:])
            # 新版 Chromium 对 v10 cookie 做域绑定：明文前 32 字节是
            # sha256(host_key)。不剥掉的话取到的是乱码，服务端会当成匿名。
            if len(plain) >= 32 and plain[:32] == hashlib.sha256(host_key.encode()).digest():
                plain = plain[32:]
            return plain.decode('utf-8', 'replace'), 'v10'
        except ValueError:
            return '', 'v10-fail'
    if value.startswith(b'v20'):
        # App-Bound Encryption：密钥需 SYSTEM 权限 + 浏览器校验服务，普通用户态无法直解
        return '', 'v20'
    try:
        plain = dpapi_decrypt(value)
        if len(plain) >= 32 and plain[:32] == hashlib.sha256(host_key.encode()).digest():
            plain = plain[32:]
        return plain.decode('utf-8', 'replace'), 'dpapi'
    except OSError:
        return '', 'legacy-fail'


def extract(user_data: str, domain: str = '163.com', cookies_db: str = '',
            save_copy: str = ''):
    """返回 (cookie_dict, report, 扫描过的库列表)。

    cookies_db 非空时直接读取已导出的副本，用于 Edge 独占锁定 Cookies 库的情况。
    save_copy 非空时把成功读出的库字节流另存一份，供下次离线使用。
    """
    key = _aes_key(user_data)
    found: dict[str, tuple[int, str, str]] = {}
    report = []
    db_paths = []
    if cookies_db:
        if not os.path.exists(cookies_db):
            raise RuntimeError('指定的 cookie 副本不存在: %s' % cookies_db)
        db_paths.append(cookies_db)
    else:
        default_db = os.path.join(user_data, 'Default', 'Network', 'Cookies')
        if os.path.exists(default_db):
            db_paths.append(default_db)
        for name in sorted(os.listdir(user_data)):
            if name.startswith('Profile '):
                p = os.path.join(user_data, name, 'Network', 'Cookies')
                if os.path.exists(p):
                    db_paths.append(p)

    for db in db_paths:
        blob = _read_locked_file(db)
        if save_copy:
            with open(save_copy, 'wb') as f:
                f.write(blob)
            print('cookie DB copy saved to %s (%d bytes)' % (save_copy, len(blob)),
                  file=sys.stderr)
        fd, tmp = tempfile.mkstemp(prefix='edge_cookies_', suffix='.db')
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(blob)
            conn = sqlite3.connect(tmp)
            rows = conn.execute(
                'select host_key,name,value,encrypted_value,path,last_access_utc '
                'from cookies where host_key like ?', ('%' + domain + '%',)).fetchall()
            conn.close()
            for host, name, plain, enc, path, accessed in rows:
                enc = enc or b''
                if plain:
                    val, ver = plain, 'plain'
                else:
                    val, ver = _decrypt_value(enc, key, host)
                report.append((host, name, ver, len(val)))
                if not val:
                    continue
                # 同名 cookie 取最近访问、且优先 path=/
                score = (1 if path == '/' else 0, accessed or 0)
                old = found.get(name)
                if old is None or score > old[0]:
                    found[name] = (score, val, host)
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass
    return {k: v[1] for k, v in found.items()}, report, db_paths


def selftest() -> None:
    # NIST AES-256-GCM 测试向量
    key = bytes(32)
    nonce = bytes(12)
    rk = _expand_key(key)
    assert _aes_encrypt_block(bytes(16), rk).hex() == 'dc95c078a2408989ad48a21492842087'
    cases = [
        (b'', '530f8afbc74536b9a963b4f1c4cb738b'),
        (bytes(16), 'd0d1c8a799996bf0265b98b5d48ab919'),
    ]
    for pt, tag in cases:
        ct = aes_gcm_decrypt(key, nonce, bytes.fromhex(
            'cea7403d4d606b6e074ec5d3baf39d18'[:len(pt) * 2]) + bytes.fromhex(tag))
        assert ct == pt
    print('AES-256-GCM selftest OK')


def main() -> int:
    ap = argparse.ArgumentParser(description='从 Edge 提取网易云 cookie')
    ap.add_argument('--user-data-dir', default=DEFAULT_USER_DATA)
    ap.add_argument('--domain', default='163.com')
    ap.add_argument('--cookies-db', default='',
                    help='直接读取已导出的 Cookies SQLite 副本（Edge 独占锁定时使用）')
    ap.add_argument('--save-copy', default='',
                    help='把成功读出的 Cookies 库另存一份，供 Edge 锁定后复用')
    ap.add_argument('--check', action='store_true', help='只列出 key/长度，不输出明文')
    ap.add_argument('--output', help='把 Cookie 头写到文件（默认写 stdout）')
    ap.add_argument('--api', help='POST 到 ebook_reader 的 /api/netease/login/cookie')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return 0

    cookies, report, dbs = extract(args.user_data_dir, args.domain,
                                   args.cookies_db, args.save_copy)
    if args.check:
        for host, name, ver, size in sorted(report, key=lambda x: (x[0], x[1])):
            if host.find('163') >= 0:
                print('%-24s %-28s %-10s len=%d' % (host, name, ver, size))
        print('---')
        print('profiles:', len(dbs), '| decrypted keys:', len(cookies))
        print('core keys:', ', '.join(k for k in CORE_KEYS if cookies.get(k)) or '(none)')
        print('v20 skipped:', sum(1 for r in report if r[2] == 'v20'))
        return 0

    order = [k for k in CORE_KEYS if cookies.get(k)]
    order += [k for k in sorted(cookies) if k not in order]
    header = '; '.join('%s=%s' % (k, cookies[k]) for k in order)
    if not header:
        print('没有解密出任何 cookie', file=sys.stderr)
        return 2

    if args.api:
        url = args.api.rstrip('/') + '/api/netease/login/cookie'
        body = json.dumps({'cookie': header}).encode('utf-8')
        req = urllib.request.Request(url, data=body,
                                     headers={'Content-Type': 'application/json'},
                                     method='POST')
        with urllib.request.urlopen(req, timeout=15) as resp:
            print(resp.read().decode('utf-8'))
    elif args.output:
        with open(args.output, 'w', encoding='utf-8') as f:
            f.write(header)
        print('written to %s (%d keys, %d bytes)' % (args.output, len(order), len(header)))
    else:
        print(header)
    return 0


if __name__ == '__main__':
    sys.exit(main())
