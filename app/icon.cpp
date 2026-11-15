// icon.cpp: the lifelog logo (see apps.h). assets/logo.svg is the same drawing; keep the two in step.

static const u8 LOGO_BLUE[3] = {0, 120, 212};  // the Windows accent
static const u8 LOGO_GREY[3] = {140, 140, 140};
static const u8 LOGO_WHITE[3] = {255, 255, 255};
static const u8 LOGO_BADGE[3] = {60, 60, 60};

// Distance-based "inside a rounded rectangle"; coordinates are 0..1 across the icon.
static bool in_round_rect(f32 x, f32 y, f32 left, f32 top, f32 right, f32 bottom, f32 radius) {
    f32 cx = (left + right) / 2, cy = (top + bottom) / 2;
    f32 hx = (right - left) / 2 - radius, hy = (bottom - top) / 2 - radius;
    f32 dx = fmaxf(fabsf(x - cx) - hx, 0), dy = fmaxf(fabsf(y - cy) - hy, 0);
    return dx * dx + dy * dy <= radius * radius;
}

static bool in_triangle(f32 x, f32 y, f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy) {
    f32 d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
    f32 d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
    f32 d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

// The colour at a point, or null outside the logo.
static const u8* logo_at(f32 x, f32 y, const u8* colour) {
    // (centre x, half height) of each bar; all centred on y = 0.41 and 0.13 wide.
    static const f32 BARS[3][2] = {{0.29f, 0.11f}, {0.50f, 0.21f}, {0.71f, 0.15f}};
    for (i32 i = 0; i < 3; i++) {
        f32 cx = BARS[i][0], half = BARS[i][1];
        if (in_round_rect(x, y, cx - 0.065f, 0.41f - half, cx + 0.065f, 0.41f + half, 0.065f)) return LOGO_WHITE;
    }
    bool bubble = in_round_rect(x, y, 0.05f, 0.07f, 0.95f, 0.75f, 0.22f);
    bool tail = in_triangle(x, y, 0.20f, 0.70f, 0.20f, 0.96f, 0.50f, 0.70f);
    return bubble || tail ? colour : 0;
}

// Private mode: the grey logo with a pause badge in the bottom-right corner.
static const u8* logo_paused_at(f32 x, f32 y) {
    f32 cx = 0.72f, cy = 0.72f, r = 0.28f;
    f32 dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy <= r * r) {
        bool left = x >= cx - 0.12f && x <= cx - 0.04f, right = x >= cx + 0.04f && x <= cx + 0.12f;
        return (left || right) && fabsf(y - cy) <= 0.13f ? LOGO_WHITE : LOGO_BADGE;
    }
    // A thin clear ring keeps the badge apart from the logo.
    if (dx * dx + dy * dy <= (r + 0.05f) * (r + 0.05f)) return 0;
    return logo_at(x, y, LOGO_GREY);
}

void logo_render(u8* out, i32 size, bool paused) {
    const i32 SUB = 4;  // 4×4 samples a pixel for smooth edges
    for (i32 py = 0; py < size; py++)
        for (i32 px = 0; px < size; px++) {
            u32 sum[3] = {}, hits = 0;
            for (i32 sy = 0; sy < SUB; sy++)
                for (i32 sx = 0; sx < SUB; sx++) {
                    f32 x = (px + (sx + 0.5f) / SUB) / size, y = (py + (sy + 0.5f) / SUB) / size;
                    const u8* c = paused ? logo_paused_at(x, y) : logo_at(x, y, LOGO_BLUE);
                    if (!c) continue;
                    hits++;
                    for (i32 i = 0; i < 3; i++) sum[i] += c[i];
                }
            u8* p = out + ((size_t)py * size + px) * 4;
            if (!hits) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            p[0] = (u8)(sum[2] / hits);
            p[1] = (u8)(sum[1] / hits);
            p[2] = (u8)(sum[0] / hits);
            p[3] = (u8)(hits * 255 / (SUB * SUB));
        }
}

HICON logo_icon(i32 size, bool paused) {
    BITMAPV5HEADER h = {};
    h.bV5Size = sizeof(h);
    h.bV5Width = size;
    h.bV5Height = -size;  // top row first
    h.bV5Planes = 1;
    h.bV5BitCount = 32;
    h.bV5Compression = BI_BITFIELDS;
    h.bV5RedMask = 0x00FF0000;
    h.bV5GreenMask = 0x0000FF00;
    h.bV5BlueMask = 0x000000FF;
    h.bV5AlphaMask = 0xFF000000;
    void* bits = 0;
    HDC dc = GetDC(0);
    HBITMAP colour = CreateDIBSection(dc, (BITMAPINFO*)&h, DIB_RGB_COLORS, &bits, 0, 0);
    ReleaseDC(0, dc);
    if (!colour) return 0;
    logo_render((u8*)bits, size, paused);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, 0);
    ICONINFO info = {TRUE, 0, 0, mask, colour};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(colour);
    return icon;
}

static u32 png_crc(const u8* data, i64 len, u32 crc) {
    for (i64 i = 0; i < len; i++) {
        crc ^= data[i];
        for (i32 k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

static u8* put_be32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v;
    return p + 4;
}

static u8* png_chunk(u8* p, const char* kind, const u8* data, u32 len) {
    p = put_be32(p, len);
    u8* start = p;
    memcpy(p, kind, 4);
    p += 4;
    if (len) memcpy(p, data, len);
    p += len;
    return put_be32(p, ~png_crc(start, 4 + len, ~0u));
}

// Uncompressed ("stored") deflate keeps the encoder to a few lines; a 64 px icon is 16 KB either way.
Str logo_png(Arena* a, i32 size) {
    u32 raw_len = (u32)(size * (size * 4 + 1));
    u32 blocks = (raw_len + 65534) / 65535;
    u32 zlib_len = 2 + blocks * 5 + raw_len + 4;
    // Pushed before the scratch mark, so it survives when `a` is the scratch arena too.
    u8* out = PUSH_ARRAY(a, u8, 8 + 25 + 12 + zlib_len + 12);
    Mark m = mark(scratch());
    u8* bgra = PUSH_ARRAY(scratch(), u8, (u64)size * size * 4);
    logo_render(bgra, size, false);
    u8* raw = PUSH_ARRAY(scratch(), u8, raw_len);
    for (i32 y = 0, at = 0; y < size; y++) {
        raw[at++] = 0;  // filter: none
        for (i32 x = 0; x < size; x++, at += 4) {
            u8* p = bgra + ((size_t)y * size + x) * 4;
            raw[at] = p[2], raw[at + 1] = p[1], raw[at + 2] = p[0], raw[at + 3] = p[3];
        }
    }
    u8* zlib = PUSH_ARRAY(scratch(), u8, zlib_len);
    u8* z = zlib;
    *z++ = 0x78, *z++ = 0x01;
    for (u32 b = 0, off = 0; b < blocks; b++) {
        u32 n = raw_len - off < 65535 ? raw_len - off : 65535;
        *z++ = b + 1 == blocks;
        *z++ = (u8)n, *z++ = (u8)(n >> 8), *z++ = (u8)~n, *z++ = (u8)(~n >> 8);
        memcpy(z, raw + off, n);
        z += n, off += n;
    }
    u32 s1 = 1, s2 = 0;
    for (u32 i = 0; i < raw_len; i++) s1 = (s1 + raw[i]) % 65521, s2 = (s2 + s1) % 65521;
    put_be32(z, (s2 << 16) | s1);

    static const u8 SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(out, SIG, 8);
    u8 header[13];
    put_be32(header, (u32)size);
    put_be32(header + 4, (u32)size);
    header[8] = 8, header[9] = 6, header[10] = header[11] = header[12] = 0;  // 8-bit RGBA
    u8* p = png_chunk(out + 8, "IHDR", header, 13);
    p = png_chunk(p, "IDAT", zlib, zlib_len);
    p = png_chunk(p, "IEND", 0, 0);
    rewind(m);
    return {(const char*)out, p - out};
}
