#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <algorithm>
#include <regex>
#include <atomic>
#include <initializer_list>

// ============================================================
// SA-MP 0.3.7-R1 "Detective - Code Decoder" auto-solver
//
// Pipeline:
//   1. Player presses Y.
//   2. We scan for the dialog for up to 5 seconds. If found (and
//      fresh — no previous-guess rows yet), we start solving.
//   3. If nothing turns up in 5 seconds, we press Delete x4 and
//      give it one more 5-second scan (10 seconds total across both
//      rounds). If that also finds nothing, we give up silently.
//   4. Once solving starts: send the next guess via simulated
//      keystrokes (verified against the edit box's real text before
//      Enter is ever pressed), wait for the dialog to reopen with an
//      extra "PREVIOUS GUESSES" row, parse its {colour}digit tokens
//      into a G/Y/R string (this REPLACES the python guess_result()
//      function — the game itself is the oracle), and feed that back
//      into the same solver state machine from the original script.
// ============================================================

// ----------------------------------------------------------------
// Log: no console window (it added overhead and wasn't useful) —
// everything goes to a plain text file instead.
// ----------------------------------------------------------------
namespace Log
{
    FILE* g_file = nullptr;

    void Init()
    {
        char tempPath[MAX_PATH] = { 0 };
        GetTempPathA(MAX_PATH, tempPath);

        std::string path = std::string(tempPath) + "samp_detective_solver.log";
        fopen_s(&g_file, path.c_str(), "w");
    }

    void Write(const char* fmt, ...)
    {
        if (!g_file)
            return;

        va_list args;
        va_start(args, fmt);
        vfprintf(g_file, fmt, args);
        va_end(args);

        fprintf(g_file, "\n");
        fflush(g_file);
    }

    void Close()
    {
        if (g_file)
        {
            fclose(g_file);
            g_file = nullptr;
        }
    }
}

// ----------------------------------------------------------------
// SampDialog: memory reading (unchanged offsets from before)
// ----------------------------------------------------------------
namespace SampDialog
{
    constexpr uintptr_t DIALOG_INFO_OFFSET = 0x21A0B8; // confirmed R1

    constexpr uintptr_t ACTIVE_OFFSET      = 0x28;
    constexpr uintptr_t TYPE_OFFSET        = 0x2C;
    constexpr uintptr_t ID_OFFSET          = 0x30;
    constexpr uintptr_t TEXT_PTR_OFFSET    = 0x34;
    constexpr uintptr_t EDITBOX_PTR_OFFSET = 0x24; // CDXUTIMEEditBox* m_pEditbox
    constexpr uintptr_t CAPTION_OFFSET     = 0x40;
    constexpr size_t    CAPTION_MAX        = 65;
    constexpr uintptr_t SERVERSIDE_OFFSET  = 0x81;

    bool IsReadable(uintptr_t address, SIZE_T size)
    {
        if (address == 0 || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};

        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & PAGE_NOACCESS)
            return false;

        if (mbi.Protect & PAGE_GUARD)
            return false;

        uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd   = regionStart + mbi.RegionSize;

        if (address > UINTPTR_MAX - size)
            return false;

        uintptr_t requestedEnd = address + size;

        if (address < regionStart)
            return false;

        if (requestedEnd > regionEnd)
            return false;

        return true;
    }

    bool ReadU8(uintptr_t address, uint8_t& value)
    {
        value = 0;
        if (!IsReadable(address, 1))
            return false;
        value = *reinterpret_cast<uint8_t*>(address);
        return true;
    }

    bool ReadU32(uintptr_t address, uint32_t& value)
    {
        value = 0;
        if (!IsReadable(address, sizeof(uint32_t)))
            return false;
        value = *reinterpret_cast<uint32_t*>(address);
        return true;
    }

    bool ReadPointer(uintptr_t address, uintptr_t& value)
    {
        value = 0;
        if (!IsReadable(address, sizeof(uintptr_t)))
            return false;
        value = *reinterpret_cast<uintptr_t*>(address);
        return true;
    }

    std::string ReadCStringSafe(uintptr_t address, size_t maxLen = 4096)
    {
        std::string result;
        if (address == 0)
            return result;

        result.reserve(64);

        for (size_t i = 0; i < maxLen; ++i)
        {
            uint8_t c = 0;
            if (!ReadU8(address + i, c))
                break;
            if (c == 0)
                break;
            result.push_back(static_cast<char>(c));
        }

        return result;
    }

    std::string ReadInlineCaption(uintptr_t dialogBase)
    {
        std::string result;
        result.reserve(CAPTION_MAX);

        uintptr_t captionAddr = dialogBase + CAPTION_OFFSET;

        for (size_t i = 0; i < CAPTION_MAX; ++i)
        {
            uint8_t c = 0;
            if (!ReadU8(captionAddr + i, c))
                break;
            if (c == 0)
                break;
            result.push_back(static_cast<char>(c));
        }

        return result;
    }

    uintptr_t GetSampBase()
    {
        HMODULE samp = GetModuleHandleA("samp.dll");
        if (!samp)
            return 0;
        return reinterpret_cast<uintptr_t>(samp);
    }

    uintptr_t GetDialogPointer()
    {
        uintptr_t sampBase = GetSampBase();
        if (!sampBase)
            return 0;

        uintptr_t globalAddress = sampBase + DIALOG_INFO_OFFSET;

        uintptr_t dialogPtr = 0;
        if (!ReadPointer(globalAddress, dialogPtr))
            return 0;

        if (dialogPtr == 0)
            return 0;

        if (dialogPtr % 4 != 0)
            return 0;

        if (!IsReadable(dialogPtr, 0x85))
            return 0;

        return dialogPtr;
    }

    bool IsDialogActive(uintptr_t dialogBase)
    {
        uint32_t active = 0;
        if (!ReadU32(dialogBase + ACTIVE_OFFSET, active))
            return false;
        return active != 0;
    }

    struct DialogSnapshot
    {
        int type = -1;
        int id = -1;
        bool serverside = false;
        std::string caption;
        std::string text;
    };

    bool GetDialogSnapshot(uintptr_t dialogBase, DialogSnapshot& out)
    {
        uint32_t type = 0, id = 0, serverside = 0, textPtr = 0;

        if (!ReadU32(dialogBase + TYPE_OFFSET, type))
            return false;

        ReadU32(dialogBase + ID_OFFSET, id);
        ReadU32(dialogBase + SERVERSIDE_OFFSET, serverside);
        ReadU32(dialogBase + TEXT_PTR_OFFSET, textPtr);

        out.type = static_cast<int>(type);
        out.id = static_cast<int>(id);
        out.serverside = (serverside != 0);
        out.caption = ReadInlineCaption(dialogBase);
        out.text = ReadCStringSafe(static_cast<uintptr_t>(textPtr));

        return true;
    }

    // ----------------------------------------------------------
    // Parses the "PREVIOUS GUESSES" section of the dialog body into
    // a list of 4-char G/Y/R strings, one per row, in order.
    //
    // Row format in the raw text:
    //   {FFFFFF}1. {FFD966}0 {FF5555}1 {FF5555}2 {FFD966}3
    //
    // {33CC33} = G   {FFD966} = Y   {FF5555} = R
    // ----------------------------------------------------------
    std::vector<std::string> ParseFeedbackRows(const std::string& text)
    {
        std::vector<std::string> rows;

        size_t startPos = text.find("PREVIOUS GUESSES");
        if (startPos == std::string::npos)
            return rows;

        size_t endPos = text.find("Enter a 4-digit combination", startPos);
        std::string section = (endPos == std::string::npos)
            ? text.substr(startPos)
            : text.substr(startPos, endPos - startPos);

        // split into lines
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char ch : section)
            {
                if (ch == '\n') { lines.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            if (!cur.empty()) lines.push_back(cur);
        }

        // matches "{RRGGBB}D" only when D is immediately followed by a
        // space or end-of-line, which excludes the "{FFFFFF}1." row
        // numbering (followed by '.') and stray digits elsewhere.
        static const std::regex tokenRegex(R"(\{([0-9A-Fa-f]{6})\}([0-9])(?=[ \n]|$))");

        for (auto& line : lines)
        {
            if (line.find(". ") == std::string::npos)
                continue; // not a "N. ..." guess row

            std::string row;
            auto begin = std::sregex_iterator(line.begin(), line.end(), tokenRegex);
            auto end = std::sregex_iterator();

            for (auto it = begin; it != end; ++it)
            {
                std::string color = (*it)[1].str();
                for (auto& c : color) c = static_cast<char>(std::toupper((unsigned char)c));

                char result = 0;
                if (color == "33CC33") result = 'G';
                else if (color == "FFD966") result = 'Y';
                else if (color == "FF5555") result = 'R';

                if (result != 0)
                    row.push_back(result);
            }

            if (row.size() == 4)
                rows.push_back(row);
        }

        return rows;
    }

    // Polls until the dialog shows `expectedAttempt` previous-guess rows,
    // then returns the G/Y/R string for that (our just-submitted) row.
    // Returns "" on timeout.
    std::string WaitForFeedback(int expectedAttempt, DWORD timeoutMs = 8000)
    {
        DWORD start = GetTickCount();

        while (GetTickCount() - start < timeoutMs)
        {
            uintptr_t dialog = GetDialogPointer();

            if (dialog && IsDialogActive(dialog))
            {
                DialogSnapshot snap;
                if (GetDialogSnapshot(dialog, snap))
                {
                    auto rows = ParseFeedbackRows(snap.text);
                    if (static_cast<int>(rows.size()) >= expectedAttempt)
                        return rows[expectedAttempt - 1];
                }
            }

            Sleep(100);
        }

        return "";
    }
}

// ----------------------------------------------------------------
// Solver: literal C++ translation of the python script's helpers.
// guess_result() and generate_random() are NOT translated — their
// job (returning the G/Y/R string, and picking a target) is now
// done by the live game via SampDialog::WaitForFeedback().
// ----------------------------------------------------------------
namespace Solver
{
    // def is_complete(correct):
    //     for g in correct:
    //         if g == "-": return False
    //     return True
    bool IsComplete(const std::array<char, 4>& correct)
    {
        for (char g : correct)
            if (g == '-')
                return false;
        return true;
    }

    // def filter_guesses(guesses, not_in_pos, correct, guessed):
    //     return list(dict.fromkeys(
    //         guess for guess in guesses
    //         if guess not in guessed
    //         and all(j not in not_in_pos[d] for j, d in enumerate(guess))
    //         and all(str(d) in guess for d in not_in_pos)
    //         and all(correct[j] == "-" or correct[j] == guess[j] for j in range(4))
    //     ))[::-1]
    std::vector<std::string> FilterGuesses(
        const std::vector<std::string>& guesses,
        std::map<char, std::vector<int>>& not_in_pos, // non-const: python's defaultdict
                                                        // auto-vivifies on read too, and we
                                                        // replicate that exactly via operator[]
        const std::array<char, 4>& correct,
        const std::vector<std::string>& guessed)
    {
        // list(dict.fromkeys(...)) : dedupe preserving first-seen order
        std::vector<std::string> unique_guesses;
        {
            std::set<std::string> seen;
            for (auto& g : guesses)
                if (seen.insert(g).second)
                    unique_guesses.push_back(g);
        }

        std::vector<std::string> filtered;

        for (auto& guess : unique_guesses)
        {
            // guess not in guessed
            if (std::find(guessed.begin(), guessed.end(), guess) != guessed.end())
                continue;

            // all(j not in not_in_pos[d] for j, d in enumerate(guess))
            bool ok = true;
            for (int j = 0; j < 4; ++j)
            {
                char d = guess[j];
                auto& forbidden = not_in_pos[d]; // auto-creates empty entry, like defaultdict
                if (std::find(forbidden.begin(), forbidden.end(), j) != forbidden.end())
                {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;

            // all(str(d) in guess for d in not_in_pos)
            for (auto& kv : not_in_pos)
            {
                if (guess.find(kv.first) == std::string::npos)
                {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;

            // all(correct[j] == "-" or correct[j] == guess[j] for j in range(4))
            for (int j = 0; j < 4; ++j)
            {
                if (!(correct[j] == '-' || correct[j] == guess[j]))
                {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;

            filtered.push_back(guess);
        }

        std::reverse(filtered.begin(), filtered.end()); // [::-1]
        return filtered;
    }

    // itertools.product(*possible_values)
    std::vector<std::vector<char>> CartesianProduct(const std::vector<std::vector<char>>& lists)
    {
        std::vector<std::vector<char>> result{ {} };

        for (auto& list : lists)
        {
            std::vector<std::vector<char>> next;
            for (auto& combo : result)
            {
                for (char item : list)
                {
                    auto newCombo = combo;
                    newCombo.push_back(item);
                    next.push_back(std::move(newCombo));
                }
            }
            result = std::move(next);
        }

        return result;
    }
}

// ----------------------------------------------------------------
// GameInput: simulated keystrokes into the game window
// ----------------------------------------------------------------
namespace GameInput
{
    struct EnumData
    {
        DWORD pid;
        HWND hwnd;
    };

    // Must be a real function (not a lambda) with the CALLBACK/__stdcall
    // convention: on the 32-bit i686-w64-mingw32 ABI, EnumWindows expects
    // a __stdcall WNDENUMPROC, and a capture-less lambda converts to a
    // plain cdecl pointer instead — GCC rejects the implicit conversion.
    BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lparam)
    {
        auto* d = reinterpret_cast<EnumData*>(lparam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        if (pid == d->pid && IsWindowVisible(hwnd))
        {
            d->hwnd = hwnd;
            return FALSE; // stop enumeration
        }
        return TRUE;
    }

    HWND FindGameWindow()
    {
        EnumData data{ GetCurrentProcessId(), nullptr };
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
        return data.hwnd;
    }

    void SendCharToWindow(HWND hwnd, char c)
    {
        PostMessageA(hwnd, WM_CHAR, static_cast<WPARAM>(c), 0);
        Sleep(15);
    }

    void SendKeyToWindow(HWND hwnd, WORD vk)
    {
        PostMessageA(hwnd, WM_KEYDOWN, vk, 0);
        Sleep(15);
        PostMessageA(hwnd, WM_KEYUP, vk, 0);
        Sleep(15);
    }

    // Confirmed for R1 via imring/SF.lua's dxut.lua:
    //   CDXUTEditBox_GetText_addr[SAMP_VERSION_037R1] = 0x81030
    // Signature: const char* GetText(CDXUTEditBox* this) — __thiscall.
    using GetTextFn = const char* (__attribute__((thiscall)) *)(void*);

    std::string GetEditBoxText(uintptr_t editBoxPtr)
    {
        if (editBoxPtr == 0 || !SampDialog::IsReadable(editBoxPtr, 4))
            return std::string();

        uintptr_t sampBase = SampDialog::GetSampBase();
        if (!sampBase)
            return std::string();

        auto GetText = reinterpret_cast<GetTextFn>(sampBase + 0x81030);
        const char* raw = GetText(reinterpret_cast<void*>(editBoxPtr));

        if (!raw)
            return std::string();

        // raw points into the game's own memory — copy it out through
        // the same byte-validated reader used everywhere else, rather
        // than trusting it's still valid/null-terminated where we think.
        return SampDialog::ReadCStringSafe(reinterpret_cast<uintptr_t>(raw), 16);
    }

    uintptr_t GetCurrentEditBoxPtr()
    {
        uintptr_t dialogBase = SampDialog::GetDialogPointer();
        if (!dialogBase)
            return 0;

        uintptr_t editBoxPtr = 0;
        SampDialog::ReadPointer(dialogBase + SampDialog::EDITBOX_PTR_OFFSET, editBoxPtr);
        return editBoxPtr;
    }

    // Types the guess in, then reads the edit box's actual text back and
    // compares it against what we meant to send. Only presses Enter once
    // they match exactly — never submits a partial/garbled guess. Retries
    // up to `maxAttempts` times on mismatch (re-clearing based on the
    // REAL current length, not an assumed fixed count) before giving up.
    // Returns false (without pressing Enter) if it can't get a clean
    // match — the caller must treat that as an abort, not a submission.
    bool TypeAndSubmitGuess(HWND hwnd, const std::string& guess, int maxAttempts = 5)
    {
        for (int attempt = 1; attempt <= maxAttempts; ++attempt)
        {
            uintptr_t editBoxPtr = GetCurrentEditBoxPtr();
            std::string current = GetEditBoxText(editBoxPtr);

            int clearCount = static_cast<int>(current.size()) + 2; // small safety margin
            for (int i = 0; i < clearCount; ++i)
                SendKeyToWindow(hwnd, VK_BACK);

            for (char c : guess)
                SendCharToWindow(hwnd, c);

            Sleep(50); // let the box settle before reading it back

            editBoxPtr = GetCurrentEditBoxPtr(); // dialog object may have moved
            std::string readBack = GetEditBoxText(editBoxPtr);

            if (readBack == guess)
            {
                SendKeyToWindow(hwnd, VK_RETURN);
                return true;
            }

            Log::Write("[Solver] Input verify failed (attempt %d/%d): wanted '%s', box has '%s' — retrying.",
                       attempt, maxAttempts, guess.c_str(), readBack.c_str());
        }

        Log::Write("[Solver] Could not reliably type '%s' after %d attempts — NOT pressing Enter.",
                   guess.c_str(), maxAttempts);
        return false;
    }
}

// ----------------------------------------------------------------
// Automation: the live solve loop
// ----------------------------------------------------------------
namespace Automation
{
    std::atomic<bool> g_solverRunning{ false };

    DWORD WINAPI SolverThread(LPVOID);

    void RunSolve(HWND hwnd)
    {
        Log::Write("[Solver] Starting solve.");

        // guesses = ["8900", "4567", "0123"]
        std::vector<std::string> guesses = { "8900", "4567", "0123" };
        std::vector<std::string> guessed;
        std::set<char> existing_nums;
        std::array<char, 4> correct = { '-', '-', '-', '-' };
        std::map<char, std::vector<int>> not_in_pos;
        std::map<char, std::vector<int>> in_pos;

        bool found = false;
        int c = 1;

        while (!found)
        {
            if (c > 7)
            {
                Log::Write("[Solver] Failed to guess in 7 tries.");
                break;
            }

            if (c > 3)
            {
                for (auto& kv : in_pos)
                    if (kv.second.size() == 1)
                        correct[kv.second[0]] = kv.first;

                if (Solver::IsComplete(correct))
                {
                    guesses.push_back(std::string(correct.begin(), correct.end()));
                    guesses = Solver::FilterGuesses(guesses, not_in_pos, correct, guessed);
                }
                else
                {
                    std::vector<int> empty;
                    for (int i = 0; i < 4; ++i)
                        if (correct[i] == '-')
                            empty.push_back(i);

                    std::vector<std::vector<char>> possible_values;
                    for (int e : empty)
                    {
                        std::vector<char> vals;
                        for (auto& kv : in_pos)
                            if (std::find(kv.second.begin(), kv.second.end(), e) != kv.second.end())
                                vals.push_back(kv.first);
                        possible_values.push_back(vals);
                    }

                    auto combos = Solver::CartesianProduct(possible_values);
                    for (auto& combo : combos)
                    {
                        for (size_t k = 0; k < empty.size(); ++k)
                            correct[empty[k]] = combo[k];

                        if (Solver::IsComplete(correct))
                            guesses.push_back(std::string(correct.begin(), correct.end()));

                        for (int e : empty)
                            correct[e] = '-';
                    }

                    guesses = Solver::FilterGuesses(guesses, not_in_pos, correct, guessed);

                    if (guesses.empty())
                    {
                        Log::Write("[Solver] No more guesses left.");
                        break;
                    }
                }
            }

            std::string guess = guesses.back();
            guesses.pop_back();
            guessed.push_back(guess);

            Log::Write("[Solver] Attempt %d: sending guess %s", c, guess.c_str());

            Sleep(200); // requested small delay before each try

            bool sent = GameInput::TypeAndSubmitGuess(hwnd, guess);

            if (!sent)
            {
                Log::Write("[Solver] Aborting — could not reliably submit the guess.");
                break;
            }

            std::string res = SampDialog::WaitForFeedback(c);

            if (res.empty())
            {
                Log::Write("[Solver] Timed out waiting for dialog feedback — aborting.");
                break;
            }

            Log::Write("[Solver] Result: %s", res.c_str());

            if (res == "GGGG")
            {
                found = true;
                Log::Write("[Solver] SOLVED on attempt %d: %s", c, guess.c_str());
                break;
            }

            for (int i = 0; i < 4; ++i)
            {
                char r = res[i];
                char d = guess[i];

                if (r == 'G')
                {
                    correct[i] = d;
                    existing_nums.insert(d);
                    in_pos[d].push_back(i);
                }
                else if (r == 'Y')
                {
                    existing_nums.insert(d);
                    not_in_pos[d].push_back(i);
                }
                // r == 'R' -> no action, mirrors the python (guess_result never
                // returns info that needs handling for red beyond doing nothing here)
            }

            for (char e : existing_nums)
            {
                std::vector<int> empty2;
                for (int i = 0; i < 4; ++i)
                    if (correct[i] == '-')
                        empty2.push_back(i);

                for (int p : empty2)
                {
                    bool pInNotInPos = std::find(not_in_pos[e].begin(), not_in_pos[e].end(), p)
                                       != not_in_pos[e].end();

                    if (!pInNotInPos)
                    {
                        if (correct[p] == '-')
                            in_pos[e].push_back(p);
                    }

                    bool pInInPos = std::find(in_pos[e].begin(), in_pos[e].end(), p)
                                    != in_pos[e].end();

                    if (pInNotInPos && pInInPos)
                    {
                        in_pos[e].erase(
                            std::remove(in_pos[e].begin(), in_pos[e].end(), p),
                            in_pos[e].end());
                    }
                }

                // not_in_pos[e] = list(set(not_in_pos[e]))
                {
                    std::set<int> s(not_in_pos[e].begin(), not_in_pos[e].end());
                    not_in_pos[e].assign(s.begin(), s.end());
                }
                // in_pos[e] = list(set(in_pos[e]))
                {
                    std::set<int> s(in_pos[e].begin(), in_pos[e].end());
                    in_pos[e].assign(s.begin(), s.end());
                }
            }

            c++;
        }
    }

    DWORD WINAPI SolverThread(LPVOID)
    {
        HWND hwnd = GameInput::FindGameWindow();

        if (!hwnd)
        {
            Log::Write("[Solver] Could not find game window.");
            g_solverRunning = false;
            return 0;
        }

        RunSolve(hwnd);

        g_solverRunning = false;
        return 0;
    }

    // Spawns the solver thread. Caller (Detection) is responsible for
    // having already confirmed a fresh dialog is present.
    void StartSolverThread()
    {
        g_solverRunning = true;
        HANDLE t = CreateThread(nullptr, 0, SolverThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

// ----------------------------------------------------------------
// Detection: bounded, Y-key-triggered scanning.
//
// Replaces the old always-on 100ms polling loop, which kept doing
// VirtualQuery/GetModuleHandle calls forever even when nothing was
// happening — cheap individually, but frequent enough (10/sec,
// forever) to cause noticeable stutter, especially under a VM where
// those are real kernel transitions.
//
// New behaviour: do nothing at all until the player presses Y. Then:
//   - scan for a fresh "Code Decoder" dialog for up to 5 seconds
//   - if not found, press Delete x4 and scan for up to 5 more seconds
//     (10 seconds total across both rounds)
//   - if still not found, give up and go back to doing nothing
// ----------------------------------------------------------------
namespace Detection
{
    std::atomic<bool> g_running{ false };

    // Returns true the moment a fresh (no previous-guess rows yet)
    // "Code Decoder" dialog is seen, polling every 100ms until
    // `windowMs` elapses.
    bool ScanForDialog(DWORD windowMs)
    {
        DWORD start = GetTickCount();

        while (GetTickCount() - start < windowMs)
        {
            uintptr_t dialog = SampDialog::GetDialogPointer();

            if (dialog && SampDialog::IsDialogActive(dialog))
            {
                SampDialog::DialogSnapshot snap;
                if (SampDialog::GetDialogSnapshot(dialog, snap))
                {
                    if (snap.caption.find("Code Decoder") != std::string::npos)
                    {
                        auto rows = SampDialog::ParseFeedbackRows(snap.text);

                        if (rows.empty())
                        {
                            Log::Write("[Detection] Fresh 'Code Decoder' dialog found.");
                            return true;
                        }
                        else
                        {
                            Log::Write("[Detection] Dialog found but already has %zu previous "
                                       "guess(es) — ignoring (would restart from scratch).",
                                       rows.size());
                        }
                    }
                }
            }

            Sleep(100);
        }

        return false;
    }

    void PressDeleteKeys(HWND hwnd, int times)
    {
        for (int i = 0; i < times; ++i)
        {
            GameInput::SendKeyToWindow(hwnd, VK_DELETE);
            Sleep(150);
        }
    }

    DWORD WINAPI SequenceThread(LPVOID)
    {
        Log::Write("[Detection] Y pressed — starting 5s scan.");

        HWND hwnd = GameInput::FindGameWindow();
        if (!hwnd)
        {
            Log::Write("[Detection] Could not find game window — aborting.");
            g_running = false;
            return 0;
        }

        bool found = ScanForDialog(5000);

        if (!found)
        {
            Log::Write("[Detection] Nothing found in first 5s — pressing Delete x4 and trying "
                       "5 more seconds (10s total).");

            PressDeleteKeys(hwnd, 4);
            found = ScanForDialog(5000);
        }

        if (found)
        {
            Automation::StartSolverThread();
        }
        else
        {
            Log::Write("[Detection] No dialog found after 10s total — giving up.");
        }

        g_running = false;
        return 0;
    }

    // Call this on a Y keypress (rising edge). No-op if a scan or a
    // solve is already in progress.
    void TriggerOnYPress()
    {
        if (g_running || Automation::g_solverRunning)
            return;

        g_running = true;
        HANDLE t = CreateThread(nullptr, 0, SequenceThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

DWORD WINAPI MainThread(LPVOID)
{
    Log::Init();
    Log::Write("SA-MP Detective Dialog Solver (0.3.7-R1) — started.");

    while (!GetModuleHandleA("gta_sa.exe"))
        Sleep(100);
    Log::Write("gta_sa.exe detected.");

    while (!GetModuleHandleA("samp.dll"))
        Sleep(100);
    Log::Write("samp.dll detected.");

    Sleep(2000); // let SA-MP finish initializing

    Log::Write("Ready. Press Y in-game to scan for the Code Decoder dialog.");

    // Lightweight key-state poll only — no memory reads happen here at
    // all unless Y is actually pressed, which is what keeps this loop
    // cheap enough to run forever.
    bool wasYDown = false;

    while (true)
    {
        bool isYDown = (GetAsyncKeyState('Y') & 0x8000) != 0;

        if (isYDown && !wasYDown)
            Detection::TriggerOnYPress();

        wasYDown = isYDown;
        Sleep(150);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Log::Close();
    }

    return TRUE;
}
