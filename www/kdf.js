'use strict';

/*
 * SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 in plain JavaScript.
 * The page runs over http, where browsers do not offer crypto.subtle; this lets
 * the login prove the password without sending it (see app.js, "challenge").
 */
window.R2Kdf = (() => {
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  const IV = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
  const W = new Uint32Array(64);

  // one 64-byte block into state h (Uint32Array(8))
  function block(h, bytes, off) {
    for (let i = 0; i < 16; i++) {
      const j = off + 4 * i;
      W[i] = (bytes[j] << 24) | (bytes[j + 1] << 16) | (bytes[j + 2] << 8) | bytes[j + 3];
    }
    for (let i = 16; i < 64; i++) {
      const a = W[i - 15], b = W[i - 2];
      const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
      const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
      W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
    }
    let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const ch = (e & f) ^ (~e & g);
      const t1 = (k + S1 + ch + K[i] + W[i]) | 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const mj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + mj) | 0;
      k = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
    h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + k) | 0;
  }

  function sha256(bytes) {
    const h = new Uint32Array(IV);
    const n = bytes.length;
    const total = ((n + 9 + 63) >> 6) << 6;
    const buf = new Uint8Array(total);
    buf.set(bytes);
    buf[n] = 0x80;
    const bits = n * 8;
    buf[total - 4] = bits >>> 24; buf[total - 3] = bits >>> 16; buf[total - 2] = bits >>> 8; buf[total - 1] = bits;
    buf[total - 5] = Math.floor(bits / 2 ** 32);
    for (let off = 0; off < total; off += 64) block(h, buf, off);
    return words(h);
  }

  function words(h) {
    const out = new Uint8Array(32);
    for (let i = 0; i < 8; i++) {
      out[4 * i] = h[i] >>> 24; out[4 * i + 1] = h[i] >>> 16; out[4 * i + 2] = h[i] >>> 8; out[4 * i + 3] = h[i];
    }
    return out;
  }

  // HMAC with the key schedule done once: state after ipad and after opad
  function hmacKey(key) {
    let k = key.length > 64 ? sha256(key) : key;
    const pad = new Uint8Array(64);
    pad.set(k);
    const ip = new Uint8Array(64), op = new Uint8Array(64);
    for (let i = 0; i < 64; i++) { ip[i] = pad[i] ^ 0x36; op[i] = pad[i] ^ 0x5c; }
    const hi = new Uint32Array(IV), ho = new Uint32Array(IV);
    block(hi, ip, 0);
    block(ho, op, 0);
    return { hi, ho };
  }

  // message after the 64-byte pad block, total length 64 + msg.length
  function finish(state, msg) {
    const h = new Uint32Array(state);
    const n = 64 + msg.length;
    const total = ((msg.length + 9 + 63) >> 6) << 6;
    const buf = new Uint8Array(total);
    buf.set(msg);
    buf[msg.length] = 0x80;
    const bits = n * 8;
    buf[total - 4] = bits >>> 24; buf[total - 3] = bits >>> 16; buf[total - 2] = bits >>> 8; buf[total - 1] = bits;
    for (let off = 0; off < total; off += 64) block(h, buf, off);
    return words(h);
  }

  function hmac(kk, msg) {
    return finish(kk.ho, finish(kk.hi, msg));
  }

  function pbkdf2(password, salt, iter) {
    const kk = hmacKey(new TextEncoder().encode(password));
    const s = new Uint8Array(salt.length + 4);
    s.set(salt);
    s[salt.length + 3] = 1;
    let u = hmac(kk, s);
    const out = u.slice();
    for (let i = 1; i < iter; i++) {
      u = hmac(kk, u);
      for (let j = 0; j < 32; j++) out[j] ^= u[j];
    }
    return out;
  }

  const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  const fromHex = (s) => new Uint8Array((s.match(/../g) || []).map((x) => parseInt(x, 16)));

  return {
    // proof for a login challenge
    proof(password, saltHex, iter, nonceHex) {
      const dk = pbkdf2(password, fromHex(saltHex), iter);
      return toHex(hmac(hmacKey(dk), fromHex(nonceHex)));
    },
    // a new password as salt + hash, so the server never sees it
    newPassword(password, iter) {
      const salt = new Uint8Array(16);
      crypto.getRandomValues(salt);
      return { salt: toHex(salt), hash: toHex(pbkdf2(password, salt, iter)), iter };
    },
    sha256Hex: (s) => toHex(sha256(new TextEncoder().encode(s))),
  };
})();
