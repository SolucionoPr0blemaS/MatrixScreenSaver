#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <ctime>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "msimg32.lib") // AlphaBlend, usado por el sistema de glifos desenfocados

// Desconexión de la librería nativa antigua de Windows para evitar conflictos de enlazado externo
#pragma comment(linker, "/NODEFAULTLIB:scrnsave.lib")

// --- STUBS OBLIGATORIOS PARA MANTENER LA COMPATIBILIDAD ---
extern "C" {
    LRESULT WINAPI ScreenSaverProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(hWnd, msg, wp, lp); }
    BOOL WINAPI ScreenSaverConfigureDialog(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) { return FALSE; }
    BOOL WINAPI RegisterDialogClasses(HANDLE hInst) { return TRUE; }
}

// --- PARÁMETROS CONFIGURABLES CON VALORES POR DEFECTO ---
int g_FontSize = 8;
int g_Density = 1480;
int g_Speed = 1;
int g_Multicolor = 0;
int g_MulticolorLine = 0;
int g_RandomSpeedCheck = 1;
int g_Quality = 0;
int g_MutateSpeed = 3;
int g_MinLength = 9;
int g_MaxLength = 31;
int g_MutablePercent = 78;
int g_ReduceGaps = 1;
int g_MinRandomSpeed = 190;
int g_MaxRandomSpeed = 320;
int g_BrightPercent = 35;
int g_BrightMode = 1;
int g_Depth3D = 1;            // Nuevo control 3D Parallax
int g_BlurZ0 = 0;
int g_BlurZ1 = 0;
int g_BlurZ2 = 0;
int g_BlurAmount = 25;
int g_Language = 0;           // 0=Espa\u00f1ol, 1=Ingl\u00e9s, 2=Franc\u00e9s, 3=Alem\u00e1n
COLORREF g_BaseColor = RGB(0, 255, 0);
wchar_t g_CharPoolStr[2048] = L"\uff71,\uff72,\uff73,\uff74,\uff75,\uff76,\uff77,\uff78,\uff79,\uff7a,\uff7b,\uff7c,\uff7d,\uff7e,\uff7f,\uff80,\uff81,\uff82,\uff83,\uff84,\uff85,\uff86,\uff87,\uff88,\uff89,\uff8a,\uff8b,\uff8c,\uff8d,\uff8e,\uff8f,\uff90,\uff91,\uff92,\uff93,\uff94,\uff95,\uff96,\uff97,\uff98,\uff99,\uff9a,\uff9b,\uff9c,\uff9d,\u20ac,0,1,2,3,4,5,6,7,8,9,$,\u4e2d,\u5929";

// --- ESTRUCTURAS DEL SISTEMA ---
struct Cell {
    wchar_t glyph;
    bool isFixed;
};

struct Chain {
    int colIdx;
    float headRow;
    float speed;
    int maxLength;
    std::vector<Cell> gridRows;
    int rainbowStart;
    int cellCounter;

    bool brightActive;
    float brightRow;
    float brightSpeed;

    int zIndex;
    bool headerBright;
};

std::vector<Chain> chains;
std::vector<bool> colOccupied;
std::vector<wchar_t> charPool;

POINT g_InitialMousePos;
bool g_MouseInitialized = false;
int g_MaxRows = 0;
int g_MaxCols = 0;
bool g_IsPreview = false;

void UpdateCharPoolFromStr() {
    charPool.clear();
    std::wstring s(g_CharPoolStr);
    size_t start = 0;
    size_t end = s.find(L',');
    while (end != std::wstring::npos) {
        std::wstring token = s.substr(start, end - start);
        if (!token.empty()) {
            size_t first = token.find_first_not_of(L" ");
            size_t last = token.find_last_not_of(L" ");
            if (first != std::wstring::npos && last != std::wstring::npos) {
                charPool.push_back(token.substr(first, (last - first + 1))[0]);
            }
        }
        start = end + 1;
        end = s.find(L',', start);
    }
    std::wstring token = s.substr(start);
    if (!token.empty()) {
        size_t first = token.find_first_not_of(L" ");
        size_t last = token.find_last_not_of(L" ");
        if (first != std::wstring::npos && last != std::wstring::npos) {
            charPool.push_back(token.substr(first, (last - first + 1))[0]);
        }
    }
    if (charPool.empty()) {
        charPool.push_back(L'M');
    }
}

COLORREF GetRainbowColor(int index) {
    float frequency = 0.4f;
    BYTE r = (BYTE)(sin(frequency * index + 0.0f) * 127 + 128);
    BYTE g = (BYTE)(sin(frequency * index + 2.09439f) * 127 + 128);
    BYTE b = (BYTE)(sin(frequency * index + 4.18879f) * 127 + 128);
    return RGB(r, g, b);
}

// ======================================================================
// --- SISTEMA DE DESENFOQUE REAL POR SPRITES (reemplaza el halo de 8 copias) ---
// ======================================================================
// El método antiguo "estampaba" el mismo glifo 8 veces en cruz/diagonal
// con distinta opacidad. Eso no es un desenfoque, es un patrón de copias
// discretas, y por eso a partir de cierto spread se ven cadenas
// duplicadas/triplicadas en vez de un trazo suave.
//
// Aquí cada glifo se renderiza UNA sola vez a un bitmap con canal alfa
// y se le aplica un blur de caja en 3 pasadas (aproximación estándar a
// un blur gaussiano). El resultado se cachea por carácter: el coste del
// desenfoque (la parte "cara") se paga una sola vez por glifo único,
// nunca por cada copia que aparece en pantalla. Cada fotograma solo
// hace un AlphaBlend (un blit) del sprite ya borroso, con el color que
// corresponda en ese instante (longitud de cola, arcoíris, brillo...).

struct BlurGlyphCacheEntry {
    std::vector<BYTE> alpha; // máscara de alfa (0-255), ya espejada en horizontal
};

static std::map<wchar_t, BlurGlyphCacheEntry> g_BlurGlyphCache;
static int g_BlurCacheFontSize = -1;
static int g_BlurCacheSpread = -1;
static int g_BlurCacheDim = -1;

static HDC     g_BlurScratchDC = NULL;
static HBITMAP g_BlurScratchBitmap = NULL;
static BYTE* g_BlurScratchBits = NULL;
static int     g_BlurScratchDim = -1;

void DestroyBlurResources() {
    if (g_BlurScratchBitmap) { DeleteObject(g_BlurScratchBitmap); g_BlurScratchBitmap = NULL; }
    if (g_BlurScratchDC) { DeleteDC(g_BlurScratchDC); g_BlurScratchDC = NULL; }
    g_BlurScratchBits = NULL;
    g_BlurScratchDim = -1;
    g_BlurGlyphCache.clear();
    g_BlurCacheFontSize = -1;
    g_BlurCacheSpread = -1;
    g_BlurCacheDim = -1;
}

// Desenfoque de caja sobre un único canal (alfa), con los bordes
// "sujetos" (clamp) para que no se oscurezcan los extremos del sprite.
// Esto se ejecuta solo al construir el sprite de un glifo nuevo, jamás
// en el bucle de dibujo de cada fotograma, así que su coste es irrelevante.
void BoxBlurPass(const std::vector<BYTE>& src, std::vector<BYTE>& dst, int w, int h, int radius) {
    if (radius <= 0) { dst = src; return; }
    std::vector<BYTE> tmp(src.size());

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= w) xx = w - 1;
                sum += src[y * w + xx];
            }
            tmp[y * w + x] = (BYTE)(sum / (radius * 2 + 1));
        }
    }
    dst.resize(src.size());
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                sum += tmp[yy * w + x];
            }
            dst[y * w + x] = (BYTE)(sum / (radius * 2 + 1));
        }
    }
}

// Renderiza un glifo a un bitmap blanco-sobre-negro, extrae el canal
// como máscara de alfa, lo espeja en horizontal (todo el sistema dibuja
// con una transformación que invierte el eje X de cada celda, así que
// espejamos aquí una vez en vez de hacerlo en cada blit) y le aplica el
// desenfoque de caja en 3 pasadas.
void BuildBlurredGlyph(wchar_t glyph, HFONT hFont, int dim, int fontSize, int spread, std::vector<BYTE>& outAlpha) {
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dim;
    bmi.bmiHeader.biHeight = -dim; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HDC tmpDC = CreateCompatibleDC(NULL);
    HBITMAP tmpBmp = CreateDIBSection(tmpDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(tmpDC, tmpBmp);

    RECT full = { 0, 0, dim, dim };
    FillRect(tmpDC, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    HFONT oldFont = (HFONT)SelectObject(tmpDC, hFont);
    SetBkMode(tmpDC, OPAQUE);
    SetBkColor(tmpDC, RGB(0, 0, 0));
    SetTextColor(tmpDC, RGB(255, 255, 255));

    int pad = (dim - fontSize) / 2;
    TextOutW(tmpDC, pad, pad, &glyph, 1);
    SelectObject(tmpDC, oldFont);

    std::vector<BYTE> raw(dim * dim);
    BYTE* px = (BYTE*)bits; // BGRA
    for (int i = 0; i < dim * dim; ++i) raw[i] = px[i * 4 + 2]; // canal R = G = B (gris) como alfa

    SelectObject(tmpDC, oldBmp);
    DeleteObject(tmpBmp);
    DeleteDC(tmpDC);

    std::vector<BYTE> flipped(dim * dim);
    for (int y = 0; y < dim; ++y)
        for (int x = 0; x < dim; ++x)
            flipped[y * dim + x] = raw[y * dim + (dim - 1 - x)];

    int boxRadius = (std::max)(1, spread / 3);
    std::vector<BYTE> tmp1, tmp2;
    BoxBlurPass(flipped, tmp1, dim, dim, boxRadius);
    BoxBlurPass(tmp1, tmp2, dim, dim, boxRadius);
    BoxBlurPass(tmp2, outAlpha, dim, dim, boxRadius);
}

// Invalida la caché cuando cambian los parámetros que afectan al
// resultado del blur (tamaño de fuente o cantidad de desenfoque).
void EnsureBlurCacheValid(int fontSize, int spread, int dim) {
    if (fontSize != g_BlurCacheFontSize || spread != g_BlurCacheSpread || dim != g_BlurCacheDim) {
        g_BlurGlyphCache.clear();
        g_BlurCacheFontSize = fontSize;
        g_BlurCacheSpread = spread;
        g_BlurCacheDim = dim;
    }
}

const std::vector<BYTE>& GetBlurredGlyph(wchar_t glyph, HFONT hFont, int dim, int fontSize, int spread) {
    auto it = g_BlurGlyphCache.find(glyph);
    if (it != g_BlurGlyphCache.end()) return it->second.alpha;

    BlurGlyphCacheEntry entry;
    BuildBlurredGlyph(glyph, hFont, dim, fontSize, spread, entry.alpha);
    auto res = g_BlurGlyphCache.emplace(glyph, std::move(entry));
    return res.first->second.alpha;
}

// Bitmap auxiliar reutilizado para teñir+mezclar cada fotograma, evita
// crear/destruir un DIB por cada carácter dibujado.
void EnsureBlurScratch(int dim) {
    if (g_BlurScratchBitmap && g_BlurScratchDim == dim) return;
    if (g_BlurScratchBitmap) { DeleteObject(g_BlurScratchBitmap); g_BlurScratchBitmap = NULL; }
    if (g_BlurScratchDC) { DeleteDC(g_BlurScratchDC); g_BlurScratchDC = NULL; }

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dim;
    bmi.bmiHeader.biHeight = -dim;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    g_BlurScratchDC = CreateCompatibleDC(NULL);
    g_BlurScratchBitmap = CreateDIBSection(g_BlurScratchDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(g_BlurScratchDC, g_BlurScratchBitmap);
    g_BlurScratchBits = (BYTE*)bits;
    g_BlurScratchDim = dim;
}

// Tiñe la máscara de alfa cacheada con el color actual (premultiplicado)
// y la mezcla sobre destDC con AlphaBlend. AlphaBlend ignora cualquier
// transformación de mundo activa (igual que BitBlt/StretchBlt), por eso
// las coordenadas de destino ya vienen calculadas en espacio de
// dispositivo desde DrawBlurredMatrixGlyph.
void BlitBlurredGlyph(HDC destDC, int destLeft, int destTop, int destW, int destH,
    const std::vector<BYTE>& alpha, int dim, COLORREF color) {
    BYTE cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
    for (int i = 0; i < dim * dim; ++i) {
        BYTE a = alpha[i];
        g_BlurScratchBits[i * 4 + 0] = (BYTE)(cb * a / 255); // B
        g_BlurScratchBits[i * 4 + 1] = (BYTE)(cg * a / 255); // G
        g_BlurScratchBits[i * 4 + 2] = (BYTE)(cr * a / 255); // R
        g_BlurScratchBits[i * 4 + 3] = a;                    // A
    }

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(destDC, destLeft, destTop, destW, destH,
        g_BlurScratchDC, 0, 0, dim, dim, bf);
}

// Calcula la posición/tamaño en espacio de dispositivo equivalente a la
// transformación de espejo+escala que usa el resto del renderizado, y
// hace el blit del glifo ya borroso con el color recibido.
void DrawBlurredMatrixGlyph(HDC destDC, HFONT hFont, wchar_t glyph, int posX, int posY,
    float scale, COLORREF color, int spread, int blurPad, int blurDim) {
    const std::vector<BYTE>& mask = GetBlurredGlyph(glyph, hFont, blurDim, blurDim - blurPad * 2, spread);

    int destW = (int)(scale * blurDim + 0.5f);
    int destH = destW;
    int destLeft = (int)(posX - scale * blurPad + 0.5f);
    int destTop = (int)(scale * posY - scale * blurPad + 0.5f);

    BlitBlurredGlyph(destDC, destLeft, destTop, destW, destH, mask, blurDim, color);
}

void LoadSettings() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Matrix_3D", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        DWORD val;
        if (RegQueryValueExW(hKey, L"FontSize", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_FontSize = val;
        if (RegQueryValueExW(hKey, L"Color", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BaseColor = val;
        if (RegQueryValueExW(hKey, L"Density", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_Density = val;
        if (RegQueryValueExW(hKey, L"Speed", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_Speed = val;
        if (RegQueryValueExW(hKey, L"Multicolor", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_Multicolor = val;
        if (RegQueryValueExW(hKey, L"MulticolorLine", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MulticolorLine = val;
        if (RegQueryValueExW(hKey, L"RandomSpeed", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_RandomSpeedCheck = val;
        if (RegQueryValueExW(hKey, L"Quality", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_Quality = val;
        if (RegQueryValueExW(hKey, L"MutateSpeed", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MutateSpeed = val;
        if (RegQueryValueExW(hKey, L"MinLength", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MinLength = val;
        if (RegQueryValueExW(hKey, L"MaxLength", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MaxLength = val;
        if (RegQueryValueExW(hKey, L"MutablePercent", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MutablePercent = val;
        if (RegQueryValueExW(hKey, L"ReduceGaps", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_ReduceGaps = val;
        if (RegQueryValueExW(hKey, L"MinRandomSpeed", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MinRandomSpeed = val;
        if (RegQueryValueExW(hKey, L"MaxRandomSpeed", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_MaxRandomSpeed = val;
        if (RegQueryValueExW(hKey, L"BrightPercent", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BrightPercent = val;
        if (RegQueryValueExW(hKey, L"BrightMode", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BrightMode = val;
        if (RegQueryValueExW(hKey, L"Depth3D", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_Depth3D = val;
        if (RegQueryValueExW(hKey, L"BlurZ0", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BlurZ0 = val;
        if (RegQueryValueExW(hKey, L"BlurZ1", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BlurZ1 = val;
        if (RegQueryValueExW(hKey, L"BlurZ2", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BlurZ2 = val;
        if (RegQueryValueExW(hKey, L"BlurAmount", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_BlurAmount = val;
        if (RegQueryValueExW(hKey, L"Language", NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) g_Language = val;

        DWORD type = REG_SZ;
        DWORD bytes = sizeof(g_CharPoolStr);
        RegQueryValueExW(hKey, L"CharPoolStr", NULL, &type, (LPBYTE)g_CharPoolStr, &bytes);
        RegCloseKey(hKey);
    }
    UpdateCharPoolFromStr();
}

void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Matrix_3D", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FontSize", 0, REG_DWORD, (const BYTE*)&g_FontSize, sizeof(DWORD));
        RegSetValueExW(hKey, L"Color", 0, REG_DWORD, (const BYTE*)&g_BaseColor, sizeof(DWORD));
        RegSetValueExW(hKey, L"Density", 0, REG_DWORD, (const BYTE*)&g_Density, sizeof(DWORD));
        RegSetValueExW(hKey, L"Speed", 0, REG_DWORD, (const BYTE*)&g_Speed, sizeof(DWORD));
        RegSetValueExW(hKey, L"Multicolor", 0, REG_DWORD, (const BYTE*)&g_Multicolor, sizeof(DWORD));
        RegSetValueExW(hKey, L"MulticolorLine", 0, REG_DWORD, (const BYTE*)&g_MulticolorLine, sizeof(DWORD));
        RegSetValueExW(hKey, L"RandomSpeed", 0, REG_DWORD, (const BYTE*)&g_RandomSpeedCheck, sizeof(DWORD));
        RegSetValueExW(hKey, L"Quality", 0, REG_DWORD, (const BYTE*)&g_Quality, sizeof(DWORD));
        RegSetValueExW(hKey, L"MutateSpeed", 0, REG_DWORD, (const BYTE*)&g_MutateSpeed, sizeof(DWORD));
        RegSetValueExW(hKey, L"MinLength", 0, REG_DWORD, (const BYTE*)&g_MinLength, sizeof(DWORD));
        RegSetValueExW(hKey, L"MaxLength", 0, REG_DWORD, (const BYTE*)&g_MaxLength, sizeof(DWORD));
        RegSetValueExW(hKey, L"MutablePercent", 0, REG_DWORD, (const BYTE*)&g_MutablePercent, sizeof(DWORD));
        RegSetValueExW(hKey, L"ReduceGaps", 0, REG_DWORD, (const BYTE*)&g_ReduceGaps, sizeof(DWORD));
        RegSetValueExW(hKey, L"MinRandomSpeed", 0, REG_DWORD, (const BYTE*)&g_MinRandomSpeed, sizeof(DWORD));
        RegSetValueExW(hKey, L"MaxRandomSpeed", 0, REG_DWORD, (const BYTE*)&g_MaxRandomSpeed, sizeof(DWORD));
        RegSetValueExW(hKey, L"BrightPercent", 0, REG_DWORD, (const BYTE*)&g_BrightPercent, sizeof(DWORD));
        RegSetValueExW(hKey, L"BrightMode", 0, REG_DWORD, (const BYTE*)&g_BrightMode, sizeof(DWORD));
        RegSetValueExW(hKey, L"Depth3D", 0, REG_DWORD, (const BYTE*)&g_Depth3D, sizeof(DWORD));
        RegSetValueExW(hKey, L"BlurZ0", 0, REG_DWORD, (const BYTE*)&g_BlurZ0, sizeof(DWORD));
        RegSetValueExW(hKey, L"BlurZ1", 0, REG_DWORD, (const BYTE*)&g_BlurZ1, sizeof(DWORD));
        RegSetValueExW(hKey, L"BlurZ2", 0, REG_DWORD, (const BYTE*)&g_BlurZ2, sizeof(DWORD));
        RegSetValueExW(hKey, L"BlurAmount", 0, REG_DWORD, (const BYTE*)&g_BlurAmount, sizeof(DWORD));
        RegSetValueExW(hKey, L"Language", 0, REG_DWORD, (const BYTE*)&g_Language, sizeof(DWORD));
        RegSetValueExW(hKey, L"CharPoolStr", 0, REG_SZ, (const BYTE*)g_CharPoolStr, (DWORD)((wcslen(g_CharPoolStr) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// ======================================================================
// --- SISTEMA DE IDIOMAS (selector de banderas en el di\u00e1logo) ---
// ======================================================================
// g_Language: 0=Espa\u00f1ol, 1=Ingl\u00e9s, 2=Franc\u00e9s, 3=Alem\u00e1n
enum UIString {
    STR_TITLE, STR_FONTSIZE, STR_DENSITY, STR_FALLSPEED, STR_QUALITY, STR_MUTATESPEED,
    STR_MUTABLEPCT, STR_MINLEN, STR_MAXLEN, STR_MINRANDOM, STR_MAXRANDOM, STR_COLORBTN,
    STR_BRIGHTPCT, STR_MULTICOLOR_LETTER, STR_MULTICOLOR_LINE, STR_RANDOMSPEED, STR_NOSPACES,
    STR_BRIGHTHEADS, STR_DEPTH3D, STR_LINE1, STR_LINE2, STR_LINE3, STR_BLURDIST, STR_CHARPOOL,
    STR_SAVE, STR_CANCEL, STR_COUNT
};

// IDs de controles que necesitan traducirse (los que ya ten\u00edan ID propio
// para su l\u00f3gica, como 201-205 o IDOK/IDCANCEL, se reutilizan tal cual).
#define ID_LBL_FONTSIZE    220
#define ID_LBL_DENSITY     221
#define ID_LBL_SPEED       222
#define ID_LBL_QUALITY     223
#define ID_LBL_MUTATE      224
#define ID_LBL_MUTABLE     225
#define ID_LBL_MINLEN      226
#define ID_LBL_MAXLEN      227
#define ID_LBL_MINRANDOM   228
#define ID_LBL_MAXRANDOM   229
#define ID_LBL_BRIGHT      230
#define ID_CHK_REDUCEGAPS  231
#define ID_CHK_BRIGHTMODE  232
#define ID_CHK_BLURZ0      233
#define ID_CHK_BLURZ1      234
#define ID_CHK_BLURZ2      235
#define ID_LBL_BLUR        236
#define ID_LBL_CHARPOOL    237
#define ID_LANG_ES         210
#define ID_LANG_EN         211
#define ID_LANG_FR         212
#define ID_LANG_DE         213
#define ID_LANG_RU         214
#define ID_LANG_JA         215

static const wchar_t* g_UIStrings[6][STR_COUNT] = {
    // --- Espa\u00f1ol ---
    {
        L"Propiedades de Matrix3D",
        L"Tama\u00f1o de Letra (P\u00edxeles):",
        L"Densidad de Cadenas:",
        L"Velocidad de Ca\u00edda:",
        L"Calidad Gr\u00e1fica (Baja - Media - Alta):",
        L"Velocidad de cambio (Mutaci\u00f3n eslabones):",
        L"Cantidad de eslabones mutables (%):",
        L"Longitud M\u00ednima de Cadena:",
        L"Longitud M\u00e1xima de Cadena:",
        L"Velocidad m\u00ednima aleatoria:",
        L"Velocidad m\u00e1xima aleatoria:",
        L"Cambiar Color Base",
        L"Cantidad de letras iluminadas (%):",
        L"Multicolor letra",
        L"Multicolor linea",
        L"Velocidad Aleatoria",
        L"No espacios",
        L"Iluminar Cabeceras",
        L"Entorno 3D",
        L"1\u00aa Linea",
        L"2\u00aa Linea",
        L"3\u00aa Linea",
        L"Desenfoque por distancias:",
        L"Caracteres de la matriz (Separa por comas):",
        L"Guardar",
        L"Cancelar",
    },
    // --- Ingl\u00e9s ---
    {
        L"Matrix3D Properties",
        L"Font Size (Pixels):",
        L"Chain Density:",
        L"Fall Speed:",
        L"Graphics Quality (Low - Medium - High):",
        L"Change Speed (Link Mutation):",
        L"Amount of Mutable Links (%):",
        L"Minimum Chain Length:",
        L"Maximum Chain Length:",
        L"Minimum Random Speed:",
        L"Maximum Random Speed:",
        L"Change Base Color",
        L"Amount of Bright Letters (%):",
        L"Multicolor letter",
        L"Multicolor line",
        L"Random Speed",
        L"No spaces",
        L"Light up Heads",
        L"3D Environment",
        L"1st Line",
        L"2nd Line",
        L"3rd Line",
        L"Blur by Distance:",
        L"Matrix Characters (Comma Separated):",
        L"Save",
        L"Cancel",
    },
    // --- Franc\u00e9s ---
    {
        L"Propri\u00e9t\u00e9s de Matrix3D",
        L"Taille de Police (Pixels):",
        L"Densit\u00e9 des Cha\u00eenes:",
        L"Vitesse de Chute:",
        L"Qualit\u00e9 Graphique (Basse - Moyenne - \u00c9lev\u00e9e):",
        L"Vitesse de Changement (Mutation des Maillons):",
        L"Quantit\u00e9 de Maillons Mutables (%):",
        L"Longueur Minimale de Cha\u00eene:",
        L"Longueur Maximale de Cha\u00eene:",
        L"Vitesse Al\u00e9atoire Minimale:",
        L"Vitesse Al\u00e9atoire Maximale:",
        L"Changer la Couleur de Base",
        L"Quantit\u00e9 de Lettres Lumineuses (%):",
        L"Lettre multicolore",
        L"Ligne multicolore",
        L"Vitesse Al\u00e9atoire",
        L"Sans espaces",
        L"\u00c9clairer les T\u00eates",
        L"Environnement 3D",
        L"1\u00e8re Ligne",
        L"2\u00e8me Ligne",
        L"3\u00e8me Ligne",
        L"Flou par Distance:",
        L"Caract\u00e8res de la Matrice (S\u00e9par\u00e9s par des Virgules):",
        L"Enregistrer",
        L"Annuler",
    },
    // --- Alem\u00e1n ---
    {
        L"Matrix3D-Eigenschaften",
        L"Schriftgr\u00f6\u00dfe (Pixel):",
        L"Kettendichte:",
        L"Fallgeschwindigkeit:",
        L"Grafikqualit\u00e4t (Niedrig - Mittel - Hoch):",
        L"\u00c4nderungsgeschwindigkeit (Gliedmutation):",
        L"Anzahl mutierbarer Glieder (%):",
        L"Minimale Kettenl\u00e4nge:",
        L"Maximale Kettenl\u00e4nge:",
        L"Minimale Zufallsgeschwindigkeit:",
        L"Maximale Zufallsgeschwindigkeit:",
        L"Basisfarbe \u00e4ndern",
        L"Anzahl leuchtender Buchstaben (%):",
        L"Mehrfarbiger Buchstabe",
        L"Mehrfarbige Linie",
        L"Zufallsgeschwindigkeit",
        L"Keine Abst\u00e4nde",
        L"K\u00f6pfe aufleuchten",
        L"3D-Umgebung",
        L"1. Linie",
        L"2. Linie",
        L"3. Linie",
        L"Unsch\u00e4rfe nach Entfernung:",
        L"Matrix-Zeichen (durch Komma getrennt):",
        L"Speichern",
        L"Abbrechen",
    },
    // --- Ruso ---
    {
        L"\u0421\u0432\u043e\u0439\u0441\u0442\u0432\u0430 Matrix3D",
        L"\u0420\u0430\u0437\u043c\u0435\u0440 \u0448\u0440\u0438\u0444\u0442\u0430 (\u043f\u0438\u043a\u0441\u0435\u043b\u0438):",
        L"\u041f\u043b\u043e\u0442\u043d\u043e\u0441\u0442\u044c \u0446\u0435\u043f\u043e\u0447\u0435\u043a:",
        L"\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c \u043f\u0430\u0434\u0435\u043d\u0438\u044f:",
        L"\u041a\u0430\u0447\u0435\u0441\u0442\u0432\u043e \u0433\u0440\u0430\u0444\u0438\u043a\u0438 (\u041d\u0438\u0437\u043a\u043e\u0435 - \u0421\u0440\u0435\u0434\u043d\u0435\u0435 - \u0412\u044b\u0441\u043e\u043a\u043e\u0435):",
        L"\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c \u0438\u0437\u043c\u0435\u043d\u0435\u043d\u0438\u044f (\u043c\u0443\u0442\u0430\u0446\u0438\u044f \u0437\u0432\u0435\u043d\u044c\u0435\u0432):",
        L"\u041a\u043e\u043b\u0438\u0447\u0435\u0441\u0442\u0432\u043e \u0438\u0437\u043c\u0435\u043d\u044f\u0435\u043c\u044b\u0445 \u0437\u0432\u0435\u043d\u044c\u0435\u0432 (%):",
        L"\u041c\u0438\u043d\u0438\u043c\u0430\u043b\u044c\u043d\u0430\u044f \u0434\u043b\u0438\u043d\u0430 \u0446\u0435\u043f\u043e\u0447\u043a\u0438:",
        L"\u041c\u0430\u043a\u0441\u0438\u043c\u0430\u043b\u044c\u043d\u0430\u044f \u0434\u043b\u0438\u043d\u0430 \u0446\u0435\u043f\u043e\u0447\u043a\u0438:",
        L"\u041c\u0438\u043d\u0438\u043c\u0430\u043b\u044c\u043d\u0430\u044f \u0441\u043b\u0443\u0447\u0430\u0439\u043d\u0430\u044f \u0441\u043a\u043e\u0440\u043e\u0441\u0442\u044c:",
        L"\u041c\u0430\u043a\u0441\u0438\u043c\u0430\u043b\u044c\u043d\u0430\u044f \u0441\u043b\u0443\u0447\u0430\u0439\u043d\u0430\u044f \u0441\u043a\u043e\u0440\u043e\u0441\u0442\u044c:",
        L"\u0418\u0437\u043c\u0435\u043d\u0438\u0442\u044c \u043e\u0441\u043d\u043e\u0432\u043d\u043e\u0439 \u0446\u0432\u0435\u0442",
        L"\u041a\u043e\u043b\u0438\u0447\u0435\u0441\u0442\u0432\u043e \u044f\u0440\u043a\u0438\u0445 \u0431\u0443\u043a\u0432 (%):",
        L"\u041c\u043d\u043e\u0433\u043e\u0446\u0432\u0435\u0442\u043d\u0430\u044f \u0431\u0443\u043a\u0432\u0430",
        L"\u041c\u043d\u043e\u0433\u043e\u0446\u0432\u0435\u0442\u043d\u0430\u044f \u043b\u0438\u043d\u0438\u044f",
        L"\u0421\u043b\u0443\u0447\u0430\u0439\u043d\u0430\u044f \u0441\u043a\u043e\u0440\u043e\u0441\u0442\u044c",
        L"\u0411\u0435\u0437 \u043f\u0440\u043e\u0431\u0435\u043b\u043e\u0432",
        L"\u041f\u043e\u0434\u0441\u0432\u0435\u0442\u043a\u0430 \u0437\u0430\u0433\u043e\u043b\u043e\u0432\u043a\u043e\u0432",
        L"3D-\u043e\u043a\u0440\u0443\u0436\u0435\u043d\u0438\u0435",
        L"1-\u044f \u043b\u0438\u043d\u0438\u044f",
        L"2-\u044f \u043b\u0438\u043d\u0438\u044f",
        L"3-\u044f \u043b\u0438\u043d\u0438\u044f",
        L"\u0420\u0430\u0437\u043c\u044b\u0442\u0438\u0435 \u043f\u043e \u0440\u0430\u0441\u0441\u0442\u043e\u044f\u043d\u0438\u044e:",
        L"\u0421\u0438\u043c\u0432\u043e\u043b\u044b \u043c\u0430\u0442\u0440\u0438\u0446\u044b (\u0447\u0435\u0440\u0435\u0437 \u0437\u0430\u043f\u044f\u0442\u0443\u044e):",
        L"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c",
        L"\u041e\u0442\u043c\u0435\u043d\u0430",
    },
    // --- Japon\u00e9s ---
    {
        L"Matrix3D \u306e\u30d7\u30ed\u30d1\u30c6\u30a3",
        L"\u30d5\u30a9\u30f3\u30c8\u30b5\u30a4\u30ba\uff08\u30d4\u30af\u30bb\u30eb\uff09\uff1a",
        L"\u30c1\u30a7\u30fc\u30f3\u306e\u5bc6\u5ea6\uff1a",
        L"\u843d\u4e0b\u901f\u5ea6\uff1a",
        L"\u30b0\u30e9\u30d5\u30a3\u30c3\u30af\u54c1\u8cea\uff08\u4f4e - \u4e2d - \u9ad8\uff09\uff1a",
        L"\u5909\u5316\u901f\u5ea6\uff08\u30ea\u30f3\u30af\u306e\u5909\u7570\uff09\uff1a",
        L"\u5909\u7570\u53ef\u80fd\u306a\u30ea\u30f3\u30af\u306e\u5272\u5408\uff08\uff05\uff09\uff1a",
        L"\u30c1\u30a7\u30fc\u30f3\u306e\u6700\u5c0f\u9577\uff1a",
        L"\u30c1\u30a7\u30fc\u30f3\u306e\u6700\u5927\u9577\uff1a",
        L"\u6700\u5c0f\u30e9\u30f3\u30c0\u30e0\u901f\u5ea6\uff1a",
        L"\u6700\u5927\u30e9\u30f3\u30c0\u30e0\u901f\u5ea6\uff1a",
        L"\u30d9\u30fc\u30b9\u30ab\u30e9\u30fc\u3092\u5909\u66f4",
        L"\u660e\u308b\u3044\u6587\u5b57\u306e\u5272\u5408\uff08\uff05\uff09\uff1a",
        L"\u6587\u5b57\u3092\u30de\u30eb\u30c1\u30ab\u30e9\u30fc",
        L"\u30e9\u30a4\u30f3\u3092\u30de\u30eb\u30c1\u30ab\u30e9\u30fc",
        L"\u30e9\u30f3\u30c0\u30e0\u901f\u5ea6",
        L"\u30b9\u30da\u30fc\u30b9\u306a\u3057",
        L"\u5148\u982d\u3092\u5149\u3089\u305b\u308b",
        L"3D\u74b0\u5883",
        L"1\u756a\u76ee\u306e\u5217",
        L"2\u756a\u76ee\u306e\u5217",
        L"3\u756a\u76ee\u306e\u5217",
        L"\u8ddd\u96e2\u306b\u3088\u308b\u307c\u304b\u3057\uff1a",
        L"\u30de\u30c8\u30ea\u30c3\u30af\u30b9\u306e\u6587\u5b57\uff08\u30ab\u30f3\u30de\u533a\u5207\u308a\uff09\uff1a",
        L"\u4fdd\u5b58",
        L"\u30ad\u30e3\u30f3\u30bb\u30eb",
    },
};

// Aplica el idioma "lang" a todos los textos del di\u00e1logo. Usa
// SetDlgItemTextW (basado en IDs de control), no en variables HWND
// capturadas, para que funcione de forma fiable sin importar desde
// d\u00f3nde se llame (incluido justo despu\u00e9s de crear los controles).
void ApplyLanguage(HWND hDlg, int lang) {
    if (lang < 0 || lang > 5) lang = 0;
    const wchar_t** s = g_UIStrings[lang];
    SetWindowTextW(hDlg, s[STR_TITLE]);
    SetDlgItemTextW(hDlg, ID_LBL_FONTSIZE, s[STR_FONTSIZE]);
    SetDlgItemTextW(hDlg, ID_LBL_DENSITY, s[STR_DENSITY]);
    SetDlgItemTextW(hDlg, ID_LBL_SPEED, s[STR_FALLSPEED]);
    SetDlgItemTextW(hDlg, ID_LBL_QUALITY, s[STR_QUALITY]);
    SetDlgItemTextW(hDlg, ID_LBL_MUTATE, s[STR_MUTATESPEED]);
    SetDlgItemTextW(hDlg, ID_LBL_MUTABLE, s[STR_MUTABLEPCT]);
    SetDlgItemTextW(hDlg, ID_LBL_MINLEN, s[STR_MINLEN]);
    SetDlgItemTextW(hDlg, ID_LBL_MAXLEN, s[STR_MAXLEN]);
    SetDlgItemTextW(hDlg, ID_LBL_MINRANDOM, s[STR_MINRANDOM]);
    SetDlgItemTextW(hDlg, ID_LBL_MAXRANDOM, s[STR_MAXRANDOM]);
    SetDlgItemTextW(hDlg, 201, s[STR_COLORBTN]);
    SetDlgItemTextW(hDlg, ID_LBL_BRIGHT, s[STR_BRIGHTPCT]);
    SetDlgItemTextW(hDlg, 202, s[STR_MULTICOLOR_LETTER]);
    SetDlgItemTextW(hDlg, 204, s[STR_MULTICOLOR_LINE]);
    SetDlgItemTextW(hDlg, 203, s[STR_RANDOMSPEED]);
    SetDlgItemTextW(hDlg, ID_CHK_REDUCEGAPS, s[STR_NOSPACES]);
    SetDlgItemTextW(hDlg, ID_CHK_BRIGHTMODE, s[STR_BRIGHTHEADS]);
    SetDlgItemTextW(hDlg, 205, s[STR_DEPTH3D]);
    SetDlgItemTextW(hDlg, ID_CHK_BLURZ0, s[STR_LINE1]);
    SetDlgItemTextW(hDlg, ID_CHK_BLURZ1, s[STR_LINE2]);
    SetDlgItemTextW(hDlg, ID_CHK_BLURZ2, s[STR_LINE3]);
    SetDlgItemTextW(hDlg, ID_LBL_BLUR, s[STR_BLURDIST]);
    SetDlgItemTextW(hDlg, ID_LBL_CHARPOOL, s[STR_CHARPOOL]);
    SetDlgItemTextW(hDlg, IDOK, s[STR_SAVE]);
    SetDlgItemTextW(hDlg, IDCANCEL, s[STR_CANCEL]);
}

// Dibuja una bandera simplificada (suficiente para un bot\u00f3n peque\u00f1o)
// dentro del rect\u00e1ngulo dado. lang: 0=ES,1=EN,2=FR,3=DE
void DrawFlag(HDC hdc, RECT rc, int lang) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    auto fillBand = [&](int x0, int y0, int x1, int y1, COLORREF color) {
        RECT band = { rc.left + x0, rc.top + y0, rc.left + x1, rc.top + y1 };
        HBRUSH br = CreateSolidBrush(color);
        FillRect(hdc, &band, br);
        DeleteObject(br);
        };

    if (lang == 2) {
        // Francia: 3 franjas verticales azul-blanco-rojo
        fillBand(0, 0, w / 3, h, RGB(0, 85, 164));
        fillBand(w / 3, 0, (2 * w) / 3, h, RGB(255, 255, 255));
        fillBand((2 * w) / 3, 0, w, h, RGB(239, 65, 53));
    }
    else if (lang == 3) {
        // Alemania: 3 franjas horizontales negro-rojo-oro
        fillBand(0, 0, w, h / 3, RGB(0, 0, 0));
        fillBand(0, h / 3, w, (2 * h) / 3, RGB(221, 0, 0));
        fillBand(0, (2 * h) / 3, w, h, RGB(255, 206, 0));
    }
    else if (lang == 4) {
        // Rusia: 3 franjas horizontales blanco-azul-rojo
        fillBand(0, 0, w, h / 3, RGB(255, 255, 255));
        fillBand(0, h / 3, w, (2 * h) / 3, RGB(0, 57, 166));
        fillBand(0, (2 * h) / 3, w, h, RGB(213, 43, 30));
    }
    else if (lang == 5) {
        // Jap\u00f3n: campo blanco con disco rojo centrado (Hinomaru)
        fillBand(0, 0, w, h, RGB(255, 255, 255));
        HBRUSH redBrush = CreateSolidBrush(RGB(188, 0, 45));
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, redBrush);
        HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
        HPEN oldPen = (HPEN)SelectObject(hdc, nullPen);
        int cx = rc.left + w / 2, cy = rc.top + h / 2;
        int rr = (int)(h * 0.32);
        Ellipse(hdc, cx - rr, cy - rr, cx + rr, cy + rr);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(redBrush);
    }
    else if (lang == 0) {
        // Espa\u00f1a: rojo-amarillo (ancho)-rojo
        fillBand(0, 0, w, h / 4, RGB(170, 21, 27));
        fillBand(0, h / 4, w, (3 * h) / 4, RGB(241, 191, 0));
        fillBand(0, (3 * h) / 4, w, h, RGB(170, 21, 27));
    }
    else {
        // Reino Unido (versi\u00f3n simplificada de la Union Jack)
        fillBand(0, 0, w, h, RGB(0, 36, 125));
        HPEN penWhiteDiag = CreatePen(PS_SOLID, (h / 5) > 0 ? (h / 5) : 1, RGB(255, 255, 255));
        HPEN oldPen = (HPEN)SelectObject(hdc, penWhiteDiag);
        MoveToEx(hdc, rc.left, rc.top, NULL); LineTo(hdc, rc.right, rc.bottom);
        MoveToEx(hdc, rc.right, rc.top, NULL); LineTo(hdc, rc.left, rc.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(penWhiteDiag);

        HPEN penRedDiag = CreatePen(PS_SOLID, (h / 9) > 0 ? (h / 9) : 1, RGB(200, 16, 46));
        oldPen = (HPEN)SelectObject(hdc, penRedDiag);
        MoveToEx(hdc, rc.left, rc.top, NULL); LineTo(hdc, rc.right, rc.bottom);
        MoveToEx(hdc, rc.right, rc.top, NULL); LineTo(hdc, rc.left, rc.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(penRedDiag);

        fillBand(0, h * 2 / 5, w, h * 3 / 5, RGB(255, 255, 255));
        fillBand(w * 2 / 5, 0, w * 3 / 5, h, RGB(255, 255, 255));
        fillBand(0, h * 9 / 20, w, h * 11 / 20, RGB(200, 16, 46));
        fillBand(w * 9 / 20, 0, w * 11 / 20, h, RGB(200, 16, 46));
    }

    FrameRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

void ResetChain(Chain& c) {
    if (c.colIdx != -1 && c.colIdx < (int)colOccupied.size()) {
        colOccupied[c.colIdx] = false;
    }

    int chosenCol = rand() % g_MaxCols;
    if (!g_RandomSpeedCheck) {
        for (int i = 0; i < g_MaxCols; ++i) {
            int testCol = (chosenCol + i) % g_MaxCols;
            if (!colOccupied[testCol]) {
                chosenCol = testCol;
                break;
            }
        }
        colOccupied[chosenCol] = true;
    }

    c.colIdx = chosenCol;
    c.rainbowStart = rand() % 500;

    if (g_MaxRows <= 6) g_MaxRows = 7;

    if (g_MaxLength < g_MinLength) g_MaxLength = g_MinLength;
    c.maxLength = g_MinLength + (rand() % (g_MaxLength - g_MinLength + 1));

    if (g_ReduceGaps) {
        c.headRow = (float)(-(rand() % (g_MaxRows / 3 + 1)));
    }
    else {
        c.headRow = (float)(-(rand() % g_MaxRows));
    }

    c.zIndex = g_Depth3D ? (rand() % 3) : 0;
    float scale = 1.0f;
    if (c.zIndex == 1) scale = 0.75f;
    else if (c.zIndex == 2) scale = 0.5f;

    int effectiveSpeedSetting = g_Speed;
    if (g_RandomSpeedCheck) {
        int minS = g_MinRandomSpeed;
        int maxS = g_MaxRandomSpeed;
        if (maxS < minS) maxS = minS;
        effectiveSpeedSetting = minS + (rand() % (maxS - minS + 1));
    }
    c.speed = (((float)effectiveSpeedSetting / 45.0f) * 0.75f + ((rand() % 8) * 0.02f)) * scale;

    c.cellCounter = 0;
    c.gridRows.assign(g_MaxRows, { L' ', true });

    c.brightActive = (rand() % 100 < g_BrightPercent);
    c.headerBright = (rand() % 100 < g_BrightPercent); // Cambio aplicado: prob basada en BrightPercent

    // Comienza estrictamente a partir del 5º por el final (evitando los 4 de atenuación)
    c.brightRow = (float)((int)c.headRow - c.maxLength + 4);
    // Multiplicador acotado de forma segura entre un mínimo de 2.5x y un máximo de 4x (15 * 0.1 = 1.5)
    float speedMultiplier = 2.5f + ((rand() % 16) * 0.1f);
    c.brightSpeed = c.speed * speedMultiplier;
}

LRESULT CALLBACK ScreenSaverWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        srand((unsigned int)time(NULL));
        LoadSettings();

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int width = rcClient.right - rcClient.left;
        int height = rcClient.bottom - rcClient.top;

        int runtimeFontSize = g_FontSize;
        if (g_IsPreview && runtimeFontSize > 8) {
            runtimeFontSize = 8;
        }

        float minScale = g_Depth3D ? 0.5f : 1.0f;
        g_MaxCols = width / runtimeFontSize;
        g_MaxRows = height / ((runtimeFontSize * minScale) + 4) + 10;
        if (g_MaxCols <= 0) g_MaxCols = 1;
        if (g_MaxRows <= 0) g_MaxRows = 1;

        colOccupied.assign(g_MaxCols, false);

        int dynamicDensity = g_Density;
        if (g_IsPreview) dynamicDensity = 35;
        if (dynamicDensity > g_MaxCols && !g_RandomSpeedCheck) dynamicDensity = g_MaxCols;

        chains.resize(dynamicDensity);
        for (int i = 0; i < dynamicDensity; ++i) {
            chains[i].colIdx = -1;
            ResetChain(chains[i]);
            chains[i].headRow = (float)(rand() % g_MaxRows) - (rand() % g_MaxRows);

            int currentHead = (int)chains[i].headRow;
            int currentTail = currentHead - chains[i].maxLength;
            if (chains[i].maxLength > 0 && chains[i].brightActive) {
                int availableRange = chains[i].maxLength - 3;
                if (availableRange < 1) availableRange = 1;
                chains[i].brightRow = (float)(currentTail + 4 + (rand() % availableRange));
            }
        }

        SetTimer(hWnd, 1, 16, NULL);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

        BYTE targetQuality = CLEARTYPE_QUALITY;
        LPCWSTR targetFontFamily = L"Meiryo";

        if (g_Quality == 0) {
            targetQuality = NONANTIALIASED_QUALITY;
            targetFontFamily = L"MS Gothic";
        }
        else if (g_Quality == 1) {
            targetQuality = ANTIALIASED_QUALITY;
        }

        int runtimeFontSize = g_FontSize;
        if (g_IsPreview && runtimeFontSize > 8) runtimeFontSize = 8;

        HFONT hFont = CreateFontW(runtimeFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, targetQuality,
            DEFAULT_PITCH | FF_DONTCARE, targetFontFamily);
        HFONT hOldFont = (HFONT)SelectObject(memDC, hFont);

        HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rc, hBlack);
        DeleteObject(hBlack);

        // Spread/desenfoque: se calcula UNA vez por fotograma (es el mismo
        // valor para todas las cadenas que tengan el blur activado, ya que
        // depende solo de g_BlurAmount y del tamaño de fuente actual).
        int globalSpread = (g_BlurAmount > 10) ? ((g_BlurAmount - 10) * runtimeFontSize) / 90 : 0;
        int blurPad = globalSpread + 4; // margen extra para que el desenfoque no se recorte en los bordes del sprite
        int blurDim = runtimeFontSize + blurPad * 2;
        if (globalSpread > 0) {
            EnsureBlurCacheValid(runtimeFontSize, globalSpread, blurDim);
            EnsureBlurScratch(blurDim);
        }

        // Se dibujan en 3 pases distintos para manejar la oclusión Z correctamente
        for (int pass = 2; pass >= 0; --pass) {
            for (const auto& ch : chains) {
                if (g_Depth3D && ch.zIndex != pass) continue;
                if (!g_Depth3D && pass != 0) continue;

                bool shouldBlur = false;
                if (g_Depth3D) {
                    if (ch.zIndex == 0 && g_BlurZ0) shouldBlur = true;
                    if (ch.zIndex == 1 && g_BlurZ1) shouldBlur = true;
                    if (ch.zIndex == 2 && g_BlurZ2) shouldBlur = true;
                }
                else {
                    if (pass == 0 && g_BlurZ0) shouldBlur = true;
                }

                int currentHead = (int)ch.headRow;
                int currentTail = currentHead - ch.maxLength;
                int posX = ch.colIdx * runtimeFontSize;

                float scale = 1.0f;
                if (g_Depth3D) {
                    if (ch.zIndex == 1) scale = 0.75f;
                    else if (ch.zIndex == 2) scale = 0.5f;
                }

                // Las cadenas con desenfoque se dibujan enteramente con
                // AlphaBlend (coordenadas ya calculadas a mano en espacio
                // de dispositivo), nunca con TextOutW. Por eso, para esas
                // cadenas NO activamos la transformación de espejo+escala.
                // OJO: lo que decide qué camino de dibujo se usa por glifo
                // es "spread > 0" (shouldBlur Y cantidad de desenfoque por
                // encima del umbral), no "shouldBlur" a secas. Si la casilla
                // de la línea está marcada pero el desenfoque está muy bajo
                // (por debajo del umbral), el glifo se sigue dibujando con
                // TextOutW y SÍ necesita la transformación activa. Usar solo
                // "shouldBlur" aquí era el bug: con la casilla marcada pero
                // desenfoque bajo, se quitaba la transformación a cadenas
                // que en realidad seguían dibujándose con TextOutW, y todas
                // acababan colapsando en x=0 (posición local sin transformar).
                bool usesBlurPath = shouldBlur && (globalSpread > 0);
                if (!usesBlurPath) {
                    XFORM xForm = { -scale, 0.0f, 0.0f, scale, (float)posX + (runtimeFontSize * scale), 0.0f };
                    SetGraphicsMode(memDC, GM_ADVANCED);
                    SetWorldTransform(memDC, &xForm);
                }

                // Configuración de intensidad por tamaño 3D
                float intensity = 1.0f;
                if (g_Depth3D) {
                    if (ch.zIndex == 1) intensity = 0.6f;
                    else if (ch.zIndex == 2) intensity = 0.3f;
                }

                for (int r = 0; r < g_MaxRows; ++r) {
                    if (r >= currentTail && r <= currentHead) {
                        if (r < 0 || r >= (int)ch.gridRows.size() || ch.gridRows[r].glyph == L' ' || ch.gridRows[r].glyph == 0) continue;

                        int posY = r * (runtimeFontSize + 4);
                        int alphaDeduction = 0;
                        if (r == currentTail)          alphaDeduction = 210;
                        else if (r == currentTail + 1) alphaDeduction = 150;
                        else if (r == currentTail + 2) alphaDeduction = 90;
                        else if (r == currentTail + 3) alphaDeduction = 40;

                        bool checkBright = false;
                        if (g_BrightMode == 1) {
                            checkBright = (ch.headerBright && r == currentHead);
                        }
                        else {
                            checkBright = (ch.brightActive && r == (int)ch.brightRow && r >= (currentTail + 4));
                        }

                        if (checkBright) {
                            COLORREF baseColorHalo = g_BaseColor;
                            if (g_Multicolor) baseColorHalo = GetRainbowColor(ch.rainbowStart + r);
                            else if (g_MulticolorLine) baseColorHalo = GetRainbowColor(ch.colIdx * 8);

                            BYTE hr = (GetRValue(baseColorHalo) + 255) / 2;
                            BYTE hg = (GetGValue(baseColorHalo) + 255) / 2;
                            BYTE hb = (GetBValue(baseColorHalo) + 255) / 2;

                            // Aplicar intensidad 3D
                            hr = (BYTE)(hr * intensity);
                            hg = (BYTE)(hg * intensity);
                            hb = (BYTE)(hb * intensity);

                            // spread=0 hasta blur>=10, luego crece: a blur=100 -> spread = runtimeFontSize
                            int spread = shouldBlur ? globalSpread : 0;
                            if (spread > 0) {
                                DrawBlurredMatrixGlyph(memDC, hFont, ch.gridRows[r].glyph, posX, posY,
                                    scale, RGB(hr, hg, hb), spread, blurPad, blurDim);
                            }
                            else {
                                SetBkColor(memDC, RGB(0, 0, 0));
                                SetBkMode(memDC, OPAQUE);
                                SetTextColor(memDC, RGB(hr, hg, hb));
                                TextOutW(memDC, -1, posY, &ch.gridRows[r].glyph, 1);

                                SetBkMode(memDC, TRANSPARENT);
                                TextOutW(memDC, 1, posY, &ch.gridRows[r].glyph, 1);
                                TextOutW(memDC, 0, posY - 1, &ch.gridRows[r].glyph, 1);
                                TextOutW(memDC, 0, posY + 1, &ch.gridRows[r].glyph, 1);

                                SetTextColor(memDC, RGB((BYTE)(255 * intensity), (BYTE)(255 * intensity), (BYTE)(255 * intensity)));
                                TextOutW(memDC, 0, posY, &ch.gridRows[r].glyph, 1);
                            }
                        }
                        else {
                            COLORREF finalColor = g_BaseColor;
                            if (g_Multicolor) finalColor = GetRainbowColor(ch.rainbowStart + r);
                            else if (g_MulticolorLine) finalColor = GetRainbowColor(ch.colIdx * 8);

                            BYTE red = GetRValue(finalColor);
                            BYTE green = GetGValue(finalColor);
                            BYTE blue = GetBValue(finalColor);

                            red = (red > alphaDeduction) ? red - alphaDeduction : 0;
                            green = (green > alphaDeduction) ? green - alphaDeduction : 0;
                            blue = (blue > alphaDeduction) ? blue - alphaDeduction : 0;

                            // Aplicar intensidad 3D
                            red = (BYTE)(red * intensity);
                            green = (BYTE)(green * intensity);
                            blue = (BYTE)(blue * intensity);

                            // spread=0 hasta blur>=10, luego crece suavemente hasta runtimeFontSize
                            int spread = shouldBlur ? globalSpread : 0;
                            if (spread > 0) {
                                DrawBlurredMatrixGlyph(memDC, hFont, ch.gridRows[r].glyph, posX, posY,
                                    scale, RGB(red, green, blue), spread, blurPad, blurDim);
                            }
                            else {
                                SetTextColor(memDC, RGB(red, green, blue));
                                SetBkColor(memDC, RGB(0, 0, 0));
                                SetBkMode(memDC, OPAQUE);
                                TextOutW(memDC, 0, posY, &ch.gridRows[r].glyph, 1);
                            }
                        }
                    }
                }
                ModifyWorldTransform(memDC, NULL, MWT_IDENTITY);
            }
        }

        BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, hOldFont);
        DeleteObject(hFont);
        SelectObject(memDC, hOldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        if (charPool.empty()) UpdateCharPoolFromStr();

        for (auto& ch : chains) {
            float prevHead = ch.headRow;
            ch.headRow += ch.speed;

            int prevHeadInt = (int)prevHead;
            int currentHeadInt = (int)ch.headRow;

            if (currentHeadInt > prevHeadInt) {
                for (int r = prevHeadInt + 1; r <= currentHeadInt; ++r) {
                    if (r >= 0 && r < g_MaxRows) {
                        ch.cellCounter++;
                        ch.gridRows[r].glyph = charPool[rand() % charPool.size()];
                        ch.gridRows[r].isFixed = (rand() % 100 >= g_MutablePercent);
                    }
                }

                int currentTailInt = currentHeadInt - ch.maxLength;
                for (int r = 0; r < g_MaxRows; ++r) {
                    if (r >= currentTailInt && r <= currentHeadInt) {
                        if (r >= 0 && r < g_MaxRows) {
                            bool isTailFade = (r >= currentTailInt && r < currentTailInt + 4);
                            if (!ch.gridRows[r].isFixed || isTailFade) {
                                ch.gridRows[r].glyph = charPool[rand() % charPool.size()];
                            }
                        }
                    }
                }
            }

            if (currentHeadInt >= 0 && currentHeadInt < g_MaxRows) {
                for (int r = currentHeadInt - ch.maxLength; r <= currentHeadInt; ++r) {
                    if (r >= 0 && r < g_MaxRows && !ch.gridRows[r].isFixed) {
                        if (rand() % 100 < g_MutateSpeed) {
                            ch.gridRows[r].glyph = charPool[rand() % charPool.size()];
                        }
                    }
                }
            }

            if (ch.brightActive && g_BrightMode == 0) {
                ch.brightRow += ch.brightSpeed;

                if (ch.brightRow > ch.headRow + 2) {
                    if (rand() % 100 < g_BrightPercent) {
                        ch.brightRow = (float)(currentHeadInt - ch.maxLength + 4);
                        float speedMultiplier = 2.5f + ((rand() % 16) * 0.1f);
                        ch.brightSpeed = ch.speed * speedMultiplier;
                    }
                    else {
                        ch.brightActive = false;
                    }
                }
            }

            if ((currentHeadInt - ch.maxLength) >= g_MaxRows) {
                ResetChain(ch);
            }
        }

        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_IsPreview) return 0;
        int mX = GET_X_LPARAM(lp);
        int mY = GET_Y_LPARAM(lp);
        if (!g_MouseInitialized) {
            g_InitialMousePos.x = mX;
            g_InitialMousePos.y = mY;
            g_MouseInitialized = true;
        }
        else if (std::abs(mX - g_InitialMousePos.x) > 15 || std::abs(mY - g_InitialMousePos.y) > 15) {
            PostQuitMessage(0);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_SYSKEYDOWN:
        if (!g_IsPreview) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        DestroyBlurResources();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// --- MENÚ DE CONFIGURACIÓN DINÁMICO ---
LRESULT CALLBACK ConfigWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND hFontTrack, hDensityTrack, hSpeedTrack, hQualityTrack, hMutateTrack, hMutableTrack, hMinLenTrack, hMaxLenTrack, hBrightTrack, hCharEdit;
    static HWND hMulticolorCheck, hMulticolorLineCheck, hRandomSpeedCheck, hReduceGapsCheck, hBrightModeCheck, hDepth3DCheck;
    static HWND hMinRandomLabel, hMinRandomTrack, hMaxRandomLabel, hMaxRandomTrack, hCharLabel, hOkButton, hCancelButton;
    static HWND hColorBtn, hBrightLabel;
    static HWND hBlurZ0Check, hBlurZ1Check, hBlurZ2Check, hBlurLabel, hBlurTrack;
    static HWND hFlagES, hFlagEN, hFlagFR, hFlagDE, hFlagRU, hFlagJA;
    static HFONT hDialogFont = NULL;

    // UpdateConfigLayout: muestra/oculta sliders de velocidad aleatoria en la columna derecha
    auto UpdateConfigLayout = [=](HWND hWindow) {
        bool isChecked = (SendMessageW(hRandomSpeedCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        int showCmd = isChecked ? SW_SHOW : SW_HIDE;
        ShowWindow(hMinRandomLabel, showCmd);
        ShowWindow(hMinRandomTrack, showCmd);
        ShowWindow(hMaxRandomLabel, showCmd);
        ShowWindow(hMaxRandomTrack, showCmd);
        // El ancho de la ventana es siempre el mismo (dos columnas fijas)
        // No se necesita redimensionar la ventana
        };

    switch (msg) {
    case WM_CREATE: {
        hDialogFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Meiryo");

        HWND hCtrl;

        // ===================== COLUMNA IZQUIERDA (x=20..305) =====================
        // Cada ítem: label en y=base, trackbar en y=base+18, siguiente ítem en base+58

        // Tamaño de Letra
        hCtrl = CreateWindowW(L"STATIC", L"Tama\u00f1o de Letra (P\u00edxeles):", WS_CHILD | WS_VISIBLE, 20, 10, 290, 18, hWnd, (HMENU)ID_LBL_FONTSIZE, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hFontTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 28, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hFontTrack, TBM_SETRANGE, TRUE, MAKELPARAM(6, 48));
        SendMessageW(hFontTrack, TBM_SETPOS, TRUE, g_FontSize);

        // Densidad de Cadenas
        hCtrl = CreateWindowW(L"STATIC", L"Densidad de Cadenas:", WS_CHILD | WS_VISIBLE, 20, 68, 290, 18, hWnd, (HMENU)ID_LBL_DENSITY, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hDensityTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 86, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hDensityTrack, TBM_SETRANGE, TRUE, MAKELPARAM(10, 5000));
        SendMessageW(hDensityTrack, TBM_SETTICFREQ, 125, 0); // menos marcas visibles; el valor sigue siendo continuo (no afecta a dónde se puede soltar el slider)
        SendMessageW(hDensityTrack, TBM_SETPOS, TRUE, g_Density);

        // Velocidad de Caída
        hCtrl = CreateWindowW(L"STATIC", L"Velocidad de Ca\u00edda:", WS_CHILD | WS_VISIBLE, 20, 126, 290, 18, hWnd, (HMENU)ID_LBL_SPEED, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hSpeedTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 144, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hSpeedTrack, TBM_SETRANGE, TRUE, MAKELPARAM(1, 400));
        SendMessageW(hSpeedTrack, TBM_SETTICFREQ, 10, 0); // menos marcas visibles; el valor sigue siendo continuo
        SendMessageW(hSpeedTrack, TBM_SETPOS, TRUE, g_Speed);

        // Calidad Gráfica
        hCtrl = CreateWindowW(L"STATIC", L"Calidad Gr\u00e1fica (Baja - Media - Alta):", WS_CHILD | WS_VISIBLE, 20, 184, 290, 18, hWnd, (HMENU)ID_LBL_QUALITY, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hQualityTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 202, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hQualityTrack, TBM_SETRANGE, TRUE, MAKELPARAM(0, 2));
        SendMessageW(hQualityTrack, TBM_SETPOS, TRUE, g_Quality);

        // Velocidad de Mutación
        hCtrl = CreateWindowW(L"STATIC", L"Velocidad de cambio (Mutaci\u00f3n eslabones):", WS_CHILD | WS_VISIBLE, 20, 242, 290, 18, hWnd, (HMENU)ID_LBL_MUTATE, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hMutateTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 260, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hMutateTrack, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
        SendMessageW(hMutateTrack, TBM_SETPOS, TRUE, g_MutateSpeed);

        // Cantidad de eslabones mutables
        hCtrl = CreateWindowW(L"STATIC", L"Cantidad de eslabones mutables (%):", WS_CHILD | WS_VISIBLE, 20, 300, 290, 18, hWnd, (HMENU)ID_LBL_MUTABLE, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hMutableTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 318, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hMutableTrack, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(hMutableTrack, TBM_SETPOS, TRUE, g_MutablePercent);

        // Longitud Mínima
        hCtrl = CreateWindowW(L"STATIC", L"Longitud M\u00ednima de Cadena:", WS_CHILD | WS_VISIBLE, 20, 358, 290, 18, hWnd, (HMENU)ID_LBL_MINLEN, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hMinLenTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 376, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hMinLenTrack, TBM_SETRANGE, TRUE, MAKELPARAM(2, 100));
        SendMessageW(hMinLenTrack, TBM_SETPOS, TRUE, g_MinLength);

        // Longitud Máxima
        hCtrl = CreateWindowW(L"STATIC", L"Longitud M\u00e1xima de Cadena:", WS_CHILD | WS_VISIBLE, 20, 416, 290, 18, hWnd, (HMENU)ID_LBL_MAXLEN, NULL, NULL);
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hMaxLenTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 15, 434, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hMaxLenTrack, TBM_SETRANGE, TRUE, MAKELPARAM(2, 100));
        SendMessageW(hMaxLenTrack, TBM_SETPOS, TRUE, g_MaxLength);

        // Botones Guardar / Cancelar (columna izquierda, abajo)
        hOkButton = CreateWindowW(L"BUTTON", L"Guardar", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 191, 490, 110, 24, hWnd, (HMENU)IDOK, NULL, NULL);
        SendMessageW(hOkButton, WM_SETFONT, (WPARAM)hDialogFont, TRUE);

        hCancelButton = CreateWindowW(L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 330, 490, 110, 24, hWnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hCancelButton, WM_SETFONT, (WPARAM)hDialogFont, TRUE);

        // ===================== COLUMNA DERECHA (x=320..610) =====================
        // Los sliders de velocidad aleatoria son opcionales (se muestran si está marcada la casilla)

        // Velocidad mínima aleatoria
        hMinRandomLabel = CreateWindowW(L"STATIC", L"Velocidad m\u00ednima aleatoria:", WS_CHILD, 320, 10, 290, 18, hWnd, (HMENU)ID_LBL_MINRANDOM, NULL, NULL);
        SendMessageW(hMinRandomLabel, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hMinRandomTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | TBS_AUTOTICKS, 315, 28, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hMinRandomTrack, TBM_SETRANGE, TRUE, MAKELPARAM(1, 400));
        SendMessageW(hMinRandomTrack, TBM_SETTICFREQ, 10, 0); // menos marcas visibles; el valor sigue siendo continuo
        SendMessageW(hMinRandomTrack, TBM_SETPOS, TRUE, g_MinRandomSpeed);

        // Velocidad máxima aleatoria
        hMaxRandomLabel = CreateWindowW(L"STATIC", L"Velocidad m\u00e1xima aleatoria:", WS_CHILD, 320, 68, 290, 18, hWnd, (HMENU)ID_LBL_MAXRANDOM, NULL, NULL);
        SendMessageW(hMaxRandomLabel, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hMaxRandomTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | TBS_AUTOTICKS, 315, 86, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hMaxRandomTrack, TBM_SETRANGE, TRUE, MAKELPARAM(1, 400));
        SendMessageW(hMaxRandomTrack, TBM_SETTICFREQ, 10, 0); // menos marcas visibles; el valor sigue siendo continuo
        SendMessageW(hMaxRandomTrack, TBM_SETPOS, TRUE, g_MaxRandomSpeed);

        // Botón color (posición fija en columna derecha — no se mueve)
        hColorBtn = CreateWindowW(L"BUTTON", L"Cambiar Color Base", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 320, 126, 290, 28, hWnd, (HMENU)201, NULL, NULL);
        SendMessageW(hColorBtn, WM_SETFONT, (WPARAM)hDialogFont, TRUE);

        // Cantidad de letras iluminadas
        hBrightLabel = CreateWindowW(L"STATIC", L"Cantidad de letras iluminadas (%):", WS_CHILD | WS_VISIBLE, 320, 168, 290, 18, hWnd, (HMENU)ID_LBL_BRIGHT, NULL, NULL);
        SendMessageW(hBrightLabel, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hBrightTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 315, 186, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hBrightTrack, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(hBrightTrack, TBM_SETPOS, TRUE, g_BrightPercent);

        // Checkboxes fila 1
        hMulticolorCheck = CreateWindowW(L"BUTTON", L"Multicolor letra", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 320, 226, 155, 20, hWnd, (HMENU)202, NULL, NULL);
        SendMessageW(hMulticolorCheck, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hMulticolorCheck, BM_SETCHECK, g_Multicolor ? BST_CHECKED : BST_UNCHECKED, 0);

        hMulticolorLineCheck = CreateWindowW(L"BUTTON", L"Multicolor linea", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFTTEXT | BS_RIGHT, 475, 226, 135, 20, hWnd, (HMENU)204, NULL, NULL);
        SendMessageW(hMulticolorLineCheck, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hMulticolorLineCheck, BM_SETCHECK, g_MulticolorLine ? BST_CHECKED : BST_UNCHECKED, 0);

        // El botón de color solo tiene sentido si NO hay multicolor activo
        EnableWindow(hColorBtn, (g_Multicolor || g_MulticolorLine) ? FALSE : TRUE);

        // Checkboxes fila 2
        hRandomSpeedCheck = CreateWindowW(L"BUTTON", L"Velocidad Aleatoria", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 320, 250, 155, 20, hWnd, (HMENU)203, NULL, NULL);
        SendMessageW(hRandomSpeedCheck, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hRandomSpeedCheck, BM_SETCHECK, g_RandomSpeedCheck ? BST_CHECKED : BST_UNCHECKED, 0);

        hReduceGapsCheck = CreateWindowW(L"BUTTON", L"No espacios", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFTTEXT | BS_RIGHT, 475, 250, 135, 20, hWnd, (HMENU)ID_CHK_REDUCEGAPS, NULL, NULL);
        SendMessageW(hReduceGapsCheck, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hReduceGapsCheck, BM_SETCHECK, g_ReduceGaps ? BST_CHECKED : BST_UNCHECKED, 0);

        // Checkboxes fila 3
        hBrightModeCheck = CreateWindowW(L"BUTTON", L"Iluminar Cabeceras", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 320, 274, 155, 20, hWnd, (HMENU)ID_CHK_BRIGHTMODE, NULL, NULL);
        SendMessageW(hBrightModeCheck, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hBrightModeCheck, BM_SETCHECK, g_BrightMode ? BST_CHECKED : BST_UNCHECKED, 0);

        hDepth3DCheck = CreateWindowW(L"BUTTON", L"Entorno 3D", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFTTEXT | BS_RIGHT, 475, 274, 135, 20, hWnd, (HMENU)205, NULL, NULL);
        SendMessageW(hDepth3DCheck, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hDepth3DCheck, BM_SETCHECK, g_Depth3D ? BST_CHECKED : BST_UNCHECKED, 0);

        // Checkboxes líneas de blur (fila 4)
        hBlurZ0Check = CreateWindowW(L"BUTTON", L"1\u00aa Linea", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 320, 298, 97, 20, hWnd, (HMENU)ID_CHK_BLURZ0, NULL, NULL);
        SendMessageW(hBlurZ0Check, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hBlurZ0Check, BM_SETCHECK, g_BlurZ0 ? BST_CHECKED : BST_UNCHECKED, 0);

        hBlurZ1Check = CreateWindowW(L"BUTTON", L"2\u00aa Linea", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 417, 298, 96, 20, hWnd, (HMENU)ID_CHK_BLURZ1, NULL, NULL);
        SendMessageW(hBlurZ1Check, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hBlurZ1Check, BM_SETCHECK, g_BlurZ1 ? BST_CHECKED : BST_UNCHECKED, 0);

        hBlurZ2Check = CreateWindowW(L"BUTTON", L"3\u00aa Linea", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFTTEXT | BS_RIGHT, 513, 298, 97, 20, hWnd, (HMENU)ID_CHK_BLURZ2, NULL, NULL);
        SendMessageW(hBlurZ2Check, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        SendMessageW(hBlurZ2Check, BM_SETCHECK, g_BlurZ2 ? BST_CHECKED : BST_UNCHECKED, 0);

        // Slider desenfoque
        hBlurLabel = CreateWindowW(L"STATIC", L"Desenfoque por distancias:", WS_CHILD | WS_VISIBLE, 320, 326, 290, 18, hWnd, (HMENU)ID_LBL_BLUR, NULL, NULL);
        SendMessageW(hBlurLabel, WM_SETFONT, (WPARAM)hDialogFont, TRUE);
        hBlurTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 315, 344, 290, 28, hWnd, NULL, NULL, NULL);
        SendMessageW(hBlurTrack, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(hBlurTrack, TBM_SETPOS, TRUE, g_BlurAmount);

        // Las líneas de desenfoque y su cantidad solo tienen sentido con Entorno 3D activo
        EnableWindow(hBlurZ0Check, g_Depth3D ? TRUE : FALSE);
        EnableWindow(hBlurZ1Check, g_Depth3D ? TRUE : FALSE);
        EnableWindow(hBlurZ2Check, g_Depth3D ? TRUE : FALSE);
        EnableWindow(hBlurTrack, g_Depth3D ? TRUE : FALSE);

        // Textarea caracteres
        hCharLabel = CreateWindowW(L"STATIC", L"Caracteres de la matriz (Separa por comas):", WS_CHILD | WS_VISIBLE, 320, 384, 290, 18, hWnd, (HMENU)ID_LBL_CHARPOOL, NULL, NULL);
        SendMessageW(hCharLabel, WM_SETFONT, (WPARAM)hDialogFont, TRUE);

        hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_CharPoolStr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
            320, 404, 290, 62, hWnd, NULL, NULL, NULL);
        SendMessageW(hCharEdit, WM_SETFONT, (WPARAM)hDialogFont, TRUE);

        // Banderas de idioma: misma fila que Guardar/Cancelar.
        // Espa\u00f1ol e Ingl\u00e9s a la izquierda de los botones, Franc\u00e9s y
        // Alem\u00e1n a la derecha. Son botones BS_OWNERDRAW (se dibujan en
        // WM_DRAWITEM) para no depender de imagenes externas.
        hFlagES = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 20, 490, 50, 24, hWnd, (HMENU)ID_LANG_ES, NULL, NULL);
        hFlagEN = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 75, 490, 50, 24, hWnd, (HMENU)ID_LANG_EN, NULL, NULL);
        hFlagRU = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 130, 490, 50, 24, hWnd, (HMENU)ID_LANG_RU, NULL, NULL);
        hFlagFR = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 450, 490, 50, 24, hWnd, (HMENU)ID_LANG_FR, NULL, NULL);
        hFlagDE = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 505, 490, 50, 24, hWnd, (HMENU)ID_LANG_DE, NULL, NULL);
        hFlagJA = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 560, 490, 50, 24, hWnd, (HMENU)ID_LANG_JA, NULL, NULL);

        UpdateConfigLayout(hWnd);
        ApplyLanguage(hWnd, g_Language);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        int flagLang = -1;
        if ((int)dis->CtlID == ID_LANG_ES) flagLang = 0;
        else if ((int)dis->CtlID == ID_LANG_EN) flagLang = 1;
        else if ((int)dis->CtlID == ID_LANG_FR) flagLang = 2;
        else if ((int)dis->CtlID == ID_LANG_DE) flagLang = 3;
        else if ((int)dis->CtlID == ID_LANG_RU) flagLang = 4;
        else if ((int)dis->CtlID == ID_LANG_JA) flagLang = 5;
        if (flagLang >= 0) {
            RECT rc = dis->rcItem;
            // La bandera se dibuja con un peque\u00f1o margen dentro del bot\u00f3n
            // para dejar sitio a un marco de selecci\u00f3n que la rodee por
            // fuera, en vez de superponerse a sus colores.
            const int margin = 3;
            RECT flagRc = { rc.left + margin, rc.top + margin, rc.right - margin, rc.bottom - margin };
            DrawFlag(dis->hDC, flagRc, flagLang);
            if (g_Language == flagLang) {
                // Resalta el idioma activo con un marco dorado MAYOR que la
                // bandera (empieza justo donde termina la bandera y ocupa
                // el espacio hasta el borde del bot\u00f3n), mismo grosor de
                // trazo que antes (2px), sin tapar los colores de dentro.
                HPEN selPen = CreatePen(PS_SOLID, 2, RGB(255, 215, 0));
                HPEN oldPen = (HPEN)SelectObject(dis->hDC, selPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(dis->hDC, oldBrush);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(selPen);
            }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wp) == ID_LANG_ES || LOWORD(wp) == ID_LANG_EN || LOWORD(wp) == ID_LANG_FR || LOWORD(wp) == ID_LANG_DE
            || LOWORD(wp) == ID_LANG_RU || LOWORD(wp) == ID_LANG_JA) {
            int newLang = (LOWORD(wp) == ID_LANG_ES) ? 0 : (LOWORD(wp) == ID_LANG_EN) ? 1 : (LOWORD(wp) == ID_LANG_FR) ? 2
                : (LOWORD(wp) == ID_LANG_DE) ? 3 : (LOWORD(wp) == ID_LANG_RU) ? 4 : 5;
            g_Language = newLang;
            ApplyLanguage(hWnd, g_Language);
            InvalidateRect(hFlagES, NULL, TRUE);
            InvalidateRect(hFlagEN, NULL, TRUE);
            InvalidateRect(hFlagFR, NULL, TRUE);
            InvalidateRect(hFlagDE, NULL, TRUE);
            InvalidateRect(hFlagRU, NULL, TRUE);
            InvalidateRect(hFlagJA, NULL, TRUE);
            return 0;
        }
        if (LOWORD(wp) == 202) {
            if (SendMessageW(hMulticolorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                SendMessageW(hMulticolorLineCheck, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            bool anyMulticolor = (SendMessageW(hMulticolorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
                (SendMessageW(hMulticolorLineCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hColorBtn, anyMulticolor ? FALSE : TRUE);
        }
        else if (LOWORD(wp) == 204) {
            if (SendMessageW(hMulticolorLineCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                SendMessageW(hMulticolorCheck, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            bool anyMulticolor = (SendMessageW(hMulticolorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ||
                (SendMessageW(hMulticolorLineCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hColorBtn, anyMulticolor ? FALSE : TRUE);
        }
        else if (LOWORD(wp) == 203) {
            UpdateConfigLayout(hWnd);
        }
        else if (LOWORD(wp) == 205) {
            bool depth3DOn = (SendMessageW(hDepth3DCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hBlurZ0Check, depth3DOn ? TRUE : FALSE);
            EnableWindow(hBlurZ1Check, depth3DOn ? TRUE : FALSE);
            EnableWindow(hBlurZ2Check, depth3DOn ? TRUE : FALSE);
            EnableWindow(hBlurTrack, depth3DOn ? TRUE : FALSE);
        }
        else if (LOWORD(wp) == 201) {
            CHOOSECOLORW cc = { 0 };
            static COLORREF custColors[16];
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hWnd;
            cc.lpCustColors = (LPDWORD)custColors;
            cc.rgbResult = g_BaseColor;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                g_BaseColor = cc.rgbResult;
            }
        }
        else if (LOWORD(wp) == IDOK) {
            g_FontSize = (int)SendMessageW(hFontTrack, TBM_GETPOS, 0, 0);
            g_Density = (int)SendMessageW(hDensityTrack, TBM_GETPOS, 0, 0);
            g_Speed = (int)SendMessageW(hSpeedTrack, TBM_GETPOS, 0, 0);
            g_Quality = (int)SendMessageW(hQualityTrack, TBM_GETPOS, 0, 0);
            g_MutateSpeed = (int)SendMessageW(hMutateTrack, TBM_GETPOS, 0, 0);
            g_MutablePercent = (int)SendMessageW(hMutableTrack, TBM_GETPOS, 0, 0);
            g_MinLength = (int)SendMessageW(hMinLenTrack, TBM_GETPOS, 0, 0);
            g_MaxLength = (int)SendMessageW(hMaxLenTrack, TBM_GETPOS, 0, 0);
            g_MinRandomSpeed = (int)SendMessageW(hMinRandomTrack, TBM_GETPOS, 0, 0);
            g_MaxRandomSpeed = (int)SendMessageW(hMaxRandomTrack, TBM_GETPOS, 0, 0);
            g_BrightPercent = (int)SendMessageW(hBrightTrack, TBM_GETPOS, 0, 0);
            g_BlurAmount = (int)SendMessageW(hBlurTrack, TBM_GETPOS, 0, 0);

            if (g_MaxLength < g_MinLength) g_MaxLength = g_MinLength;
            if (g_MaxRandomSpeed < g_MinRandomSpeed) g_MaxRandomSpeed = g_MinRandomSpeed;

            GetWindowTextW(hCharEdit, g_CharPoolStr, 2048);

            g_Multicolor = (SendMessageW(hMulticolorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_MulticolorLine = (SendMessageW(hMulticolorLineCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_RandomSpeedCheck = (SendMessageW(hRandomSpeedCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_ReduceGaps = (SendMessageW(hReduceGapsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_BrightMode = (SendMessageW(hBrightModeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_Depth3D = (SendMessageW(hDepth3DCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_BlurZ0 = (SendMessageW(hBlurZ0Check, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_BlurZ1 = (SendMessageW(hBlurZ1Check, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            g_BlurZ2 = (SendMessageW(hBlurZ2Check, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

            SaveSettings();
            EndDialog(hWnd, IDOK);
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wp) == IDCANCEL) {
            EndDialog(hWnd, IDCANCEL);
            DestroyWindow(hWnd);
        }
        return 0;
    }
    case WM_DESTROY:
        if (hDialogFont) DeleteObject(hDialogFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    LoadSettings();
    InitCommonControls();

    std::string arguments = cmd ? cmd : "";
    HWND hParentWnd = NULL;

    size_t pPos = arguments.find("/p");
    if (pPos == std::string::npos) pPos = arguments.find("/P");
    if (pPos == std::string::npos) pPos = arguments.find("-p");
    if (pPos == std::string::npos) pPos = arguments.find("-P");

    if (pPos != std::string::npos) {
        g_IsPreview = true;
        std::string hwndStr = "";
        size_t idx = pPos + 2;
        while (idx < arguments.size() && (arguments[idx] == ' ' || arguments[idx] == ':')) {
            idx++;
        }
        while (idx < arguments.size() && isdigit((unsigned char)arguments[idx])) {
            hwndStr += arguments[idx];
            idx++;
        }
        if (!hwndStr.empty()) {
            hParentWnd = (HWND)(ULONG_PTR)std::stoull(hwndStr);
        }
    }

    if (!arguments.empty() && (arguments.find("/c") == 0 || arguments.find("/C") == 0 || hParentWnd == NULL && g_IsPreview == false && arguments.find("/s") == std::string::npos && arguments.find("/S") == std::string::npos)) {
        WNDCLASSW wcc = { 0 };
        wcc.lpfnWndProc = (WNDPROC)ConfigWndProc;
        wcc.hInstance = hInst;
        wcc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
        wcc.lpszClassName = L"MatrixConfigWindow";
        RegisterClassW(&wcc);

        int winWidth = 640;
        int winHeight = 560;
        int scrWidth = GetSystemMetrics(SM_CXSCREEN);
        int scrHeight = GetSystemMetrics(SM_CYSCREEN);
        int posX = (scrWidth - winWidth) / 2;
        int posY = (scrHeight - winHeight) / 2;

        HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MatrixConfigWindow", L"Propiedades de Matrix3D",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            posX, posY, winWidth, winHeight, NULL, NULL, hInst, NULL);

        ShowWindow(hDlg, SW_SHOW);
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        return 0;
    }

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ScreenSaverWndProc;
    wc.hInstance = hInst;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MatrixSaverGraphics";
    RegisterClassW(&wc);

    HWND hwnd;
    if (g_IsPreview && hParentWnd != NULL) {
        RECT parentRect;
        GetClientRect(hParentWnd, &parentRect);
        hwnd = CreateWindowExW(0, L"MatrixSaverGraphics", L"", WS_CHILD | WS_VISIBLE,
            0, 0, parentRect.right - parentRect.left, parentRect.bottom - parentRect.top,
            hParentWnd, NULL, hInst, NULL);
    }
    else {
        hwnd = CreateWindowExW(WS_EX_TOPMOST, L"MatrixSaverGraphics", L"", WS_POPUP | WS_VISIBLE,
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), NULL, NULL, hInst, NULL);
        ShowCursor(FALSE);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { DispatchMessage(&msg); }

    if (!g_IsPreview) ShowCursor(TRUE);
    return 0;
}