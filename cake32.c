/*
 * CAKE32 - minimal Win32/x86 port of Charles Petzold's November 1985 CAKE.
 *
 * Design goals:
 *   - PE32/x86 only; no 64-bit code or pointers.
 *   - ANSI API only (UNICODE is forbidden), keeping the Win32s path viable.
 *   - All menus, dialogs, and resource captions remain English.
 *   - User-entered cake text may be Korean on CP949 or UTF-8-ACP systems.
 *   - No C runtime dependency: private PRNG and custom PE entry point.
 *   - Windows 1-3 timer quantization is modelled explicitly.
 */

#include <windows.h>
#include "cake32.h"

#ifdef UNICODE
#error CAKE32_LEGACY must be compiled with ANSI Win32 APIs (UNICODE undefined).
#endif

#define CANVAS_WIDTH             640
#define CANVAS_HEIGHT            480
#define MAX_CAKE_TEXT_CHARS       79
#define MAX_CAKE_TEXT_BYTES      321
#define MAX_CANDLE_COUNT         0xFFFFFFFEUL

/* IBM PC timer: 1,193,182 Hz / 65,536 = 18.20651245 Hz. */
#define LEGACY_TICK_SECONDS       0.05492540115422458
#define TIMER_PUMP_ID             1
#define TIMER_PUMP_REQUEST_MS     1
#define FLASH_INTERVAL_TICKS      4  /* Windows 1-3 rounds 250 ms down to 4 ticks. */
#define CANDLE_SPAN               560UL
#define CANDLE_LEFT               40UL

/* Compiler marker for x87 floating-point instructions in a freestanding
   image. The timer uses only basic double arithmetic; no CRT is linked. */
int _fltused = 0;

typedef struct CAKE_COUNTER {
    DWORD LowPart;
    LONG HighPart;
} CAKE_COUNTER;

typedef BOOL (WINAPI *PFN_QUERY_PERFORMANCE_COUNTER)(CAKE_COUNTER *);
typedef BOOL (WINAPI *PFN_QUERY_PERFORMANCE_FREQUENCY)(CAKE_COUNTER *);
typedef UINT (WINAPI *PFN_GET_ACP)(void);

typedef struct FIREWORK_STATE {
    short xOrigin;
    short yOrigin;
    short x[8];
    short y[8];
    short step;
    COLORREF color[8];
} FIREWORK_STATE;

static HINSTANCE g_instance;
static BOOL g_koreanACP = TRUE;
static BOOL g_utf8ACP = FALSE;
static BOOL g_candlesBurning = FALSE;
static BOOL g_flashAndFireworks = FALSE;
static BOOL g_pumpActive = FALSE;
static UINT g_candleCount = 1;
static char g_cakeText[MAX_CAKE_TEXT_BYTES] = "<text>";
static HBITMAP g_flameBitmaps[3];
static HFONT g_cakeFont;
static int g_textFlashCount;
static int g_flashTickCount;
static DWORD g_randomState = 1;

static const POINT g_cakeLeft  = { 40, 125 };
static const POINT g_cakeRight = { 600, 275 };
static const short g_cakeHeight = 50;
static const COLORREF g_candleColors[3] = {
    RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255)
};

static const short g_fireworkXIncrement[8] = {
    -1, 0, 1, -1, 1, -1, 0, 1
};
static const short g_fireworkYIncrement[8] = {
    -1, -1, -1, 0, 0, 1, 1, 1
};
static FIREWORK_STATE g_firework;

static PFN_QUERY_PERFORMANCE_COUNTER g_queryCounter;
static PFN_QUERY_PERFORMANCE_FREQUENCY g_queryFrequency;
static double g_qpcFrequency;
static double g_qpcInterval;
static double g_qpcLast;
static BOOL g_qpcReady = FALSE;

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static BOOL CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);

static void CopyText(char *destination, const char *source, int capacity)
{
    int i;
    if (capacity <= 0)
        return;
    for (i = 0; i < capacity - 1 && source[i] != '\0'; ++i)
        destination[i] = source[i];
    destination[i] = '\0';
}

static unsigned int RandomValue(void)
{
    /* Same family as the classic Microsoft C runtime rand(), but private. */
    g_randomState = g_randomState * 214013UL + 2531011UL;
    return (unsigned int)((g_randomState >> 16) & 0x7FFFUL);
}

static unsigned int RandomBelow(unsigned int limit)
{
    if (limit == 0)
        return 0;
    return RandomValue() % limit;
}

static double CounterToDouble(const CAKE_COUNTER *counter)
{
    return ((double)counter->HighPart * 4294967296.0) +
           (double)counter->LowPart;
}

static void DiscoverOptionalAPIs(void)
{
    HMODULE kernel;
    FARPROC proc;
    PFN_GET_ACP getACP;
    CAKE_COUNTER frequency;

    /* GetACP is absent on Win32s. Korean Win3.1/Win32s is the intended
       legacy target, so an absent GetACP means CP949. Modern Windows has it. */
    g_koreanACP = TRUE;
    kernel = GetModuleHandle("KERNEL32.DLL");
    if (kernel == NULL)
        return;

    proc = GetProcAddress(kernel, "GetACP");
    if (proc != NULL) {
        getACP = (PFN_GET_ACP)proc;
        {
            UINT acp;
            acp = getACP();
            g_koreanACP = (acp == 949);
            g_utf8ACP = (acp == 65001);
        }
    }

    /* QPC is optional. It is deliberately resolved at runtime so a Win32s
       loader never has to resolve an API that it does not implement. */
    proc = GetProcAddress(kernel, "QueryPerformanceCounter");
    if (proc != NULL)
        g_queryCounter = (PFN_QUERY_PERFORMANCE_COUNTER)proc;

    proc = GetProcAddress(kernel, "QueryPerformanceFrequency");
    if (proc != NULL)
        g_queryFrequency = (PFN_QUERY_PERFORMANCE_FREQUENCY)proc;

    if (g_queryCounter != NULL && g_queryFrequency != NULL &&
        g_queryFrequency(&frequency) &&
        (frequency.HighPart > 0 || frequency.LowPart != 0)) {
        g_qpcFrequency = CounterToDouble(&frequency);
        g_qpcInterval = g_qpcFrequency * LEGACY_TICK_SECONDS;
        g_qpcReady = TRUE;
    }
}

static void ResetLegacyClock(void)
{
    CAKE_COUNTER current;
    if (g_qpcReady && g_queryCounter(&current))
        g_qpcLast = CounterToDouble(&current);
    else
        g_qpcReady = FALSE;
}

static BOOL LegacyTickDue(void)
{
    CAKE_COUNTER current;
    double now;
    double elapsed;

    /* On Win32s, a 1 ms SetTimer request is quantized by the 18.2 Hz
       system clock. Each WM_TIMER therefore already represents one tick. */
    if (!g_qpcReady)
        return TRUE;

    if (!g_queryCounter(&current)) {
        g_qpcReady = FALSE;
        return TRUE;
    }

    now = CounterToDouble(&current);
    elapsed = now - g_qpcLast;
    if (elapsed < g_qpcInterval)
        return FALSE;

    /* Keep the 18.2065 Hz phase instead of accumulating host timer rounding.
       If the UI thread was blocked for >1.5 ticks, treat the timer as the
       coalesced WM_TIMER that classic Windows would deliver, not a catch-up. */
    if (elapsed > (g_qpcInterval * 1.5))
        g_qpcLast = now;
    else
        g_qpcLast += g_qpcInterval;

    return TRUE;
}

static void SetCanvasMapping(HDC hdc, HWND hwnd)
{
    RECT client;
    int clientWidth;
    int clientHeight;
    int viewWidth;
    int viewHeight;

    if (!GetClientRect(hwnd, &client))
        return;
    clientWidth = client.right - client.left;
    clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0)
        return;

    if ((long)clientWidth * CANVAS_HEIGHT >
        (long)clientHeight * CANVAS_WIDTH) {
        viewHeight = clientHeight;
        viewWidth = (int)((long)viewHeight * CANVAS_WIDTH / CANVAS_HEIGHT);
    } else {
        viewWidth = clientWidth;
        viewHeight = (int)((long)viewWidth * CANVAS_HEIGHT / CANVAS_WIDTH);
    }

    SetMapMode(hdc, MM_ANISOTROPIC);
    SetWindowOrgEx(hdc, 0, 0, NULL);
    SetWindowExtEx(hdc, CANVAS_WIDTH, CANVAS_HEIGHT, NULL);
    SetViewportExtEx(hdc, viewWidth, viewHeight, NULL);
    SetViewportOrgEx(hdc, (clientWidth - viewWidth) / 2,
                     (clientHeight - viewHeight) / 2, NULL);
}

static void DrawCakeText(HDC hdc, COLORREF color)
{
    RECT textRect;
    int oldMode;
    COLORREF oldColor;
    HGDIOBJ oldFont;

    textRect.left = g_cakeLeft.x + 50;
    textRect.top = g_cakeLeft.y + g_cakeHeight / 2;
    textRect.right = g_cakeRight.x - 50;
    textRect.bottom = g_cakeRight.y;

    oldFont = SelectObject(hdc, g_cakeFont);
    oldMode = SetBkMode(hdc, TRANSPARENT);
    oldColor = SetTextColor(hdc, color);

    DrawText(hdc, g_cakeText, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    OffsetRect(&textRect, 1, 1);
    DrawText(hdc, g_cakeText, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK);

    SetTextColor(hdc, oldColor);
    SetBkMode(hdc, oldMode);
    SelectObject(hdc, oldFont);
}

static void DrawCake(HDC hdc)
{
    HBRUSH icingBrush;
    int saved;

    saved = SaveDC(hdc);
    if (saved == 0)
        return;

    icingBrush = CreateSolidBrush(RGB(255, 192, 192));
    if (icingBrush == NULL) {
        RestoreDC(hdc, saved);
        return;
    }

    SelectObject(hdc, icingBrush);
    Ellipse(hdc, g_cakeLeft.x,
            g_cakeRight.y - g_cakeHeight / 2,
            g_cakeRight.x, g_cakeRight.y + g_cakeHeight / 2);

    SelectObject(hdc, GetStockObject(NULL_PEN));
    Rectangle(hdc, g_cakeLeft.x, g_cakeLeft.y,
              g_cakeRight.x, g_cakeRight.y);

    /* The top ellipse is icing-coloured in the original Petzold sample. */
    SelectObject(hdc, GetStockObject(BLACK_PEN));
    Ellipse(hdc, g_cakeLeft.x,
            g_cakeLeft.y - g_cakeHeight / 2,
            g_cakeRight.x, g_cakeLeft.y + g_cakeHeight / 2);

    SelectObject(hdc, GetStockObject(BLACK_BRUSH));
    MoveToEx(hdc, g_cakeLeft.x, g_cakeLeft.y, NULL);
    LineTo(hdc, g_cakeLeft.x, g_cakeRight.y);
    MoveToEx(hdc, g_cakeRight.x, g_cakeLeft.y, NULL);
    LineTo(hdc, g_cakeRight.x, g_cakeRight.y);

    RestoreDC(hdc, saved);
    DeleteObject(icingBrush);
}

static void DrawCandle(HDC hdc, COLORREF color, short x, short y)
{
    HBRUSH candleBrush;
    int saved;

    saved = SaveDC(hdc);
    if (saved == 0)
        return;
    candleBrush = CreateSolidBrush(color);
    if (candleBrush == NULL) {
        RestoreDC(hdc, saved);
        return;
    }

    SelectObject(hdc, candleBrush);
    Rectangle(hdc, x - 5, y - 75, x + 5, y);
    SelectObject(hdc, GetStockObject(WHITE_PEN));
    SelectObject(hdc, GetStockObject(WHITE_BRUSH));
    Rectangle(hdc, x - 2, y - 75, x + 2, y - 91);

    RestoreDC(hdc, saved);
    DeleteObject(candleBrush);
}

/*
 * The original source draws one candle per requested count.  Its x position
 * simplifies exactly to:
 *
 *   x = 40 + floor(560 * (index + 1) / (count + 1))
 *
 * More than 560 candles can share an x coordinate.  At a given x, candles
 * with the same index parity have identical geometry, so only the last one
 * of each parity affects the final pixels.  This keeps drawing bounded by
 * 560 columns (at most 1120 candles) without imposing an arbitrary count cap.
 * The quotient/remainder form avoids 64-bit arithmetic and overflow.
 */
static BOOL GetLastCandleIndex(UINT count, UINT xOffset,
                               UINT indexParity, UINT *indexOut)
{
    DWORD denominator;
    DWORD quotient;
    DWORD remainder;
    DWORD a;
    DWORD lower;
    DWORD upper;

    if (count == 0 || xOffset >= CANDLE_SPAN || indexOut == NULL)
        return FALSE;

    denominator = (DWORD)count + 1UL;
    quotient = denominator / CANDLE_SPAN;
    remainder = denominator % CANDLE_SPAN;

    a = (DWORD)xOffset;
    lower = a * quotient + (a * remainder + CANDLE_SPAN - 1UL) /
                                      CANDLE_SPAN;
    if (lower < 1UL)
        lower = 1UL;

    a = (DWORD)xOffset + 1UL;
    upper = a * quotient + (a * remainder + CANDLE_SPAN - 1UL) /
                                      CANDLE_SPAN;
    if (upper == 0UL)
        return FALSE;
    --upper;
    if (upper > count)
        upper = count;

    /* index = j - 1; even indices require odd j, and vice versa. */
    if ((upper & 1UL) != (indexParity == 0 ? 1UL : 0UL)) {
        if (upper == 0UL)
            return FALSE;
        --upper;
    }

    if (upper < lower)
        return FALSE;

    *indexOut = (UINT)(upper - 1UL);
    return TRUE;
}

static void DrawOneCandle(HDC hdc, UINT index, int x)
{
    short y;
    y = (short)g_cakeLeft.y;
    if ((index & 1UL) != 0UL)
        y = (short)(y - 5);
    else
        y = (short)(y + 5);
    DrawCandle(hdc, g_candleColors[index % 3UL], (short)x, y);
}

static void DrawCakeScene(HDC hdc)
{
    UINT xOffset;
    DrawCake(hdc);
    DrawCakeText(hdc, RGB(0, 0, 0));

    for (xOffset = 0; xOffset < CANDLE_SPAN; ++xOffset) {
        UINT evenIndex;
        UINT oddIndex;
        BOOL haveEven;
        BOOL haveOdd;
        int x;

        haveEven = GetLastCandleIndex(g_candleCount, xOffset, 0,
                                      &evenIndex);
        haveOdd = GetLastCandleIndex(g_candleCount, xOffset, 1,
                                     &oddIndex);
        x = (int)(CANDLE_LEFT + xOffset);

        if (haveEven && haveOdd) {
            if (evenIndex < oddIndex) {
                DrawOneCandle(hdc, evenIndex, x);
                DrawOneCandle(hdc, oddIndex, x);
            } else {
                DrawOneCandle(hdc, oddIndex, x);
                DrawOneCandle(hdc, evenIndex, x);
            }
        } else if (haveEven) {
            DrawOneCandle(hdc, evenIndex, x);
        } else if (haveOdd) {
            DrawOneCandle(hdc, oddIndex, x);
        }
    }
}

static HBITMAP CreateFlameBitmap(HDC referenceDC, int index)
{
    static const int outerPosition[3] = { 0, 7, 15 };
    static const int innerPosition[3] = { 3, 7, 11 };
    POINT outer[3] = { { 0, 0 }, { 0, 15 }, { 15, 15 } };
    POINT inner[4] = {
        { 7, 10 }, { 3, 17 }, { 7, 20 }, { 11, 17 }
    };
    RECT bitmapRect;
    HBITMAP bitmap;
    HBITMAP oldBitmap;
    HDC memoryDC;
    HBRUSH yellowBrush;
    HBRUSH redBrush;
    HGDIOBJ oldBrush;
    HGDIOBJ oldPen;

    bitmapRect.left = 0;
    bitmapRect.top = 0;
    bitmapRect.right = 15;
    bitmapRect.bottom = 25;

    bitmap = CreateCompatibleBitmap(referenceDC, 15, 25);
    if (bitmap == NULL)
        return NULL;
    memoryDC = CreateCompatibleDC(referenceDC);
    if (memoryDC == NULL) {
        DeleteObject(bitmap);
        return NULL;
    }

    oldBitmap = (HBITMAP)SelectObject(memoryDC, bitmap);
    if (oldBitmap == NULL || oldBitmap == (HBITMAP)HGDI_ERROR) {
        DeleteDC(memoryDC);
        DeleteObject(bitmap);
        return NULL;
    }

    FillRect(memoryDC, &bitmapRect,
             (HBRUSH)GetStockObject(BLACK_BRUSH));
    yellowBrush = CreateSolidBrush(RGB(255, 255, 0));
    redBrush = CreateSolidBrush(RGB(255, 0, 0));
    if (yellowBrush == NULL || redBrush == NULL) {
        if (yellowBrush != NULL)
            DeleteObject(yellowBrush);
        if (redBrush != NULL)
            DeleteObject(redBrush);
        SelectObject(memoryDC, oldBitmap);
        DeleteDC(memoryDC);
        DeleteObject(bitmap);
        return NULL;
    }

    outer[0].x = outerPosition[index];
    inner[0].x = innerPosition[index];
    oldBrush = SelectObject(memoryDC, yellowBrush);
    oldPen = SelectObject(memoryDC, GetStockObject(NULL_PEN));
    Ellipse(memoryDC, 0, 10, 15, 25);
    Polygon(memoryDC, outer, 3);
    SelectObject(memoryDC, redBrush);
    Polygon(memoryDC, inner, 4);

    SelectObject(memoryDC, oldPen);
    SelectObject(memoryDC, oldBrush);
    DeleteObject(yellowBrush);
    DeleteObject(redBrush);
    SelectObject(memoryDC, oldBitmap);
    DeleteDC(memoryDC);
    return bitmap;
}

static BOOL CreateGraphicsResources(HWND hwnd)
{
    HDC hdc;
    int i;

    hdc = GetDC(hwnd);
    if (hdc == NULL)
        return FALSE;

    for (i = 0; i < 3; ++i) {
        g_flameBitmaps[i] = CreateFlameBitmap(hdc, i);
        if (g_flameBitmaps[i] == NULL) {
            ReleaseDC(hwnd, hdc);
            return FALSE;
        }
    }
    ReleaseDC(hwnd, hdc);

    if (g_koreanACP || g_utf8ACP) {
        g_cakeFont = CreateFont(60, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 HANGUL_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "");
    } else {
        g_cakeFont = CreateFont(67, 0, 0, 0, 700, FALSE, FALSE, FALSE,
                                 OEM_CHARSET, 0, 0, 0, 0, "Script");
    }
    return (g_cakeFont != NULL);
}

static void DestroyGraphicsResources(void)
{
    int i;
    for (i = 0; i < 3; ++i) {
        if (g_flameBitmaps[i] != NULL) {
            DeleteObject(g_flameBitmaps[i]);
            g_flameBitmaps[i] = NULL;
        }
    }
    if (g_cakeFont != NULL) {
        DeleteObject(g_cakeFont);
        g_cakeFont = NULL;
    }
}

static void DrawFlames(HDC hdc)
{
    HDC memoryDC;
    HBITMAP oldBitmap;
    UINT xOffset;

    memoryDC = CreateCompatibleDC(hdc);
    if (memoryDC == NULL)
        return;
    oldBitmap = (HBITMAP)SelectObject(memoryDC, g_flameBitmaps[0]);
    if (oldBitmap == NULL || oldBitmap == (HBITMAP)HGDI_ERROR) {
        DeleteDC(memoryDC);
        return;
    }

    for (xOffset = 0; xOffset < CANDLE_SPAN; ++xOffset) {
        UINT evenIndex;
        UINT oddIndex;
        BOOL haveEven;
        BOOL haveOdd;
        int x;

        haveEven = GetLastCandleIndex(g_candleCount, xOffset, 0,
                                      &evenIndex);
        haveOdd = GetLastCandleIndex(g_candleCount, xOffset, 1,
                                     &oddIndex);
        x = (int)(CANDLE_LEFT + xOffset);

        if (haveEven && haveOdd && evenIndex < oddIndex) {
            SelectObject(memoryDC, g_flameBitmaps[RandomBelow(3)]);
            BitBlt(hdc, x - 7, g_cakeLeft.y + 5 - 115, 15, 25,
                   memoryDC, 0, 0, SRCCOPY);
            SelectObject(memoryDC, g_flameBitmaps[RandomBelow(3)]);
            BitBlt(hdc, x - 7, g_cakeLeft.y - 5 - 115, 15, 25,
                   memoryDC, 0, 0, SRCCOPY);
        } else if (haveEven && haveOdd) {
            SelectObject(memoryDC, g_flameBitmaps[RandomBelow(3)]);
            BitBlt(hdc, x - 7, g_cakeLeft.y - 5 - 115, 15, 25,
                   memoryDC, 0, 0, SRCCOPY);
            SelectObject(memoryDC, g_flameBitmaps[RandomBelow(3)]);
            BitBlt(hdc, x - 7, g_cakeLeft.y + 5 - 115, 15, 25,
                   memoryDC, 0, 0, SRCCOPY);
        } else if (haveEven) {
            SelectObject(memoryDC, g_flameBitmaps[RandomBelow(3)]);
            BitBlt(hdc, x - 7, g_cakeLeft.y + 5 - 115, 15, 25,
                   memoryDC, 0, 0, SRCCOPY);
        } else if (haveOdd) {
            SelectObject(memoryDC, g_flameBitmaps[RandomBelow(3)]);
            BitBlt(hdc, x - 7, g_cakeLeft.y - 5 - 115, 15, 25,
                   memoryDC, 0, 0, SRCCOPY);
        }
    }

    SelectObject(memoryDC, oldBitmap);
    DeleteDC(memoryDC);
}

static void DrawFirework(HDC hdc)
{
    int i;
    if ((g_firework.step % 6) == 0) {
        g_firework.xOrigin = (short)RandomBelow(CANVAS_WIDTH);
        g_firework.yOrigin = (short)RandomBelow(125);
        g_firework.step = 0;
        for (i = 0; i < 8; ++i) {
            g_firework.x[i] = (short)(g_firework.xOrigin +
                                      g_fireworkXIncrement[i]);
            g_firework.y[i] = (short)(g_firework.yOrigin +
                                      g_fireworkYIncrement[i]);
        }
    } else {
        for (i = 0; i < 8; ++i) {
            SetPixel(hdc, g_firework.x[i], g_firework.y[i],
                     g_firework.color[i]);
            g_firework.x[i] = (short)(g_firework.x[i] +
                                      g_fireworkXIncrement[i] *
                                      g_firework.step);
            g_firework.y[i] = (short)(g_firework.y[i] +
                                      g_fireworkYIncrement[i] *
                                      g_firework.step);
        }
    }

    if ((g_firework.step % 6) != 5) {
        for (i = 0; i < 8; ++i) {
            g_firework.color[i] = GetPixel(hdc, g_firework.x[i],
                                            g_firework.y[i]);
            SetPixel(hdc, g_firework.x[i], g_firework.y[i],
                     RGB(255, 255, 255));
        }
    }
    ++g_firework.step;
}

static void StartTimerPump(HWND hwnd)
{
    if (g_pumpActive)
        return;

    ResetLegacyClock();
    if (SetTimer(hwnd, TIMER_PUMP_ID, TIMER_PUMP_REQUEST_MS, NULL) != 0)
        g_pumpActive = TRUE;
    else
        MessageBox(hwnd, "Could not start the animation timer.",
                    "Birthday Cake", MB_OK | MB_ICONEXCLAMATION);
}

static void UpdateTimerPump(HWND hwnd)
{
    BOOL required;
    required = (g_candlesBurning || g_flashAndFireworks);

    if (required)
        StartTimerPump(hwnd);
    else if (g_pumpActive) {
        KillTimer(hwnd, TIMER_PUMP_ID);
        g_pumpActive = FALSE;
    }
}

static BOOL CALLBACK SettingsProc(HWND hdlg, UINT message,
                                  WPARAM wParam, LPARAM lParam)
{
    (void)lParam;

    switch (message) {
    case WM_INITDIALOG:
        SetDlgItemText(hdlg, ID_CAKETEXT, g_cakeText);
        SetDlgItemInt(hdlg, ID_NUMCANDLES, g_candleCount, FALSE);
        SendDlgItemMessage(hdlg, ID_FLASH, BM_SETCHECK,
                            g_flashAndFireworks ? 1 : 0, 0L);
        SendDlgItemMessage(hdlg, ID_CAKETEXT, EM_LIMITTEXT,
                            MAX_CAKE_TEXT_CHARS, 0L);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            char newText[MAX_CAKE_TEXT_BYTES];
            BOOL translated;
            UINT count;

            newText[0] = '\0';
            GetDlgItemText(hdlg, ID_CAKETEXT, newText,
                            MAX_CAKE_TEXT_BYTES);
            count = GetDlgItemInt(hdlg, ID_NUMCANDLES,
                                  &translated, FALSE);
            if (!translated || count > MAX_CANDLE_COUNT) {
                MessageBox(hdlg,
                    "Enter a non-negative integer up to 4294967294.",
                    "Cake Settings",
                    MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(hdlg, ID_NUMCANDLES));
                return TRUE;
            }

            CopyText(g_cakeText, newText, MAX_CAKE_TEXT_BYTES);
            g_candleCount = count;
            g_flashAndFireworks =
                (SendDlgItemMessage(hdlg, ID_FLASH, BM_GETCHECK,
                                     0, 0L) != 0);
            g_flashTickCount = 0;
            EndDialog(hdlg, TRUE);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hdlg, FALSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        if (!CreateGraphicsResources(hwnd)) {
            DestroyGraphicsResources();
            MessageBox(hwnd, "Could not create the cake graphics resources.",
                       "Birthday Cake", MB_OK | MB_ICONERROR);
            return -1;
        }
        SetFocus(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SETTINGS:
            if (DialogBoxParam(g_instance,
                                MAKEINTRESOURCE(IDD_SETTINGS),
                                hwnd, SettingsProc, 0) == TRUE) {
                UpdateTimerPump(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;

        case IDM_BURNING:
            g_candlesBurning = TRUE;
            UpdateTimerPump(hwnd);
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc;
        int saved;
        hdc = BeginPaint(hwnd, &ps);
        saved = SaveDC(hdc);
        if (saved != 0) {
            SetCanvasMapping(hdc, hwnd);
            DrawCakeScene(hdc);
            RestoreDC(hdc, saved);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_PUMP_ID && LegacyTickDue()) {
            HDC hdc;
            int saved;
            hdc = GetDC(hwnd);
            if (hdc != NULL) {
                saved = SaveDC(hdc);
                if (saved != 0) {
                    SetCanvasMapping(hdc, hwnd);
                    if (g_candlesBurning)
                        DrawFlames(hdc);
                    if (g_flashAndFireworks) {
                        DrawFirework(hdc);
                        ++g_flashTickCount;
                        if (g_flashTickCount >= FLASH_INTERVAL_TICKS) {
                            DrawCakeText(hdc,
                                (g_textFlashCount % 4) ? RGB(0, 0, 0) :
                                                         RGB(255, 0, 0));
                            ++g_textFlashCount;
                            g_flashTickCount = 0;
                        }
                    }
                    RestoreDC(hdc, saved);
                }
                ReleaseDC(hwnd, hdc);
            }
        }
        return 0;

    case WM_DESTROY:
        if (g_pumpActive) {
            KillTimer(hwnd, TIMER_PUMP_ID);
            g_pumpActive = FALSE;
        }
        DestroyGraphicsResources();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS windowClass;
    HWND hwnd;
    RECT windowRect;
    DWORD windowStyle;
    MSG message;
    int result;

    (void)hPrevInstance;
    (void)lpCmdLine;

    g_instance = hInstance;
    DiscoverOptionalAPIs();
    CopyText(g_cakeText, "<text>", MAX_CAKE_TEXT_BYTES);
    g_randomState = GetTickCount() ^ 0xA5A55A5AUL;
    if (g_randomState == 0)
        g_randomState = 1;

    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.cbClsExtra = 0;
    windowClass.cbWndExtra = 0;
    windowClass.hInstance = hInstance;
    windowClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    windowClass.lpszMenuName = MAKEINTRESOURCE(IDR_CAKE_MENU);
    windowClass.lpszClassName = "PetzoldCake32";

    if (!RegisterClass(&windowClass)) {
        MessageBox(NULL, "Could not register the window class.",
                   "Birthday Cake", MB_OK | MB_ICONERROR);
        return 1;
    }

    windowStyle = WS_OVERLAPPEDWINDOW;
    windowRect.left = 0;
    windowRect.top = 0;
    windowRect.right = CANVAS_WIDTH;
    windowRect.bottom = CANVAS_HEIGHT;
    if (!AdjustWindowRect(&windowRect, windowStyle, TRUE)) {
        MessageBox(NULL, "Could not calculate the window size.",
                   "Birthday Cake", MB_OK | MB_ICONERROR);
        return 1;
    }

    hwnd = CreateWindow(
        "PetzoldCake32",
        "Birthday Cake",
        windowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        NULL, NULL, hInstance, NULL);

    if (hwnd == NULL) {
        MessageBox(NULL, "Could not create the application window.",
                   "Birthday Cake", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    for (;;) {
        result = (int)GetMessage(&message, NULL, 0, 0);
        if (result == 0)
            break;
        if (result == -1) {
            MessageBox(hwnd, "An error occurred in the message loop.",
                       "Birthday Cake", MB_OK | MB_ICONERROR);
            return 1;
        }
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return (int)message.wParam;
}

/* Custom PE entry point removes the dependency on a modern C runtime DLL. */
void CakeEntry(void)
{
    HINSTANCE instance;
    int exitCode;
    instance = GetModuleHandle(NULL);
    exitCode = WinMain(instance, NULL, "", SW_SHOWNORMAL);
    ExitProcess((UINT)exitCode);
}
