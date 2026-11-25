using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Threading;
using MonoCore;
using static MonoCore.E3ImGUI;

namespace Sentinel
{
    // Core grants a single worker turn per pulse. While it waits, the client
    // thread services read requests. The worker never calls the native SDK.
    public sealed class HailstoneHost : IGameHost
    {
        // HS_AbilityKinds in sdk/hailstone_plugin.h.
        private const int HS_ABILITY_DISCIPLINE = 1;
        private const int HS_ABILITY_ALT = 2;

        private sealed class Read
        {
            internal Func<string> Fetch;
            internal string Value;
            internal ExceptionDispatchInfo Error;
            internal readonly ManualResetEventSlim Done = new ManualResetEventSlim();
        }

        private readonly ConcurrentQueue<Read> _reads = new ConcurrentQueue<Read>();
        private readonly AutoResetEvent _readReady = new AutoResetEvent(false);
        private readonly int _clientThread = Thread.CurrentThread.ManagedThreadId;
        private volatile bool _threadLent;
        // Capture once on the client thread before granting the worker. Reads
        // outside this list (including readiness) keep their normal live path.
        private static readonly string[] WorkerStateFields =
        {
            "${Me.ID}", "${Me.Dead}", "${Me.Hovering}", "${Me.Stunned}", "${Me.Silenced}",
            "${Me.PctHPs}", "${Me.PctMana}", "${Me.PctEndurance}",
            "${Me.Casting.ID}", "${Me.Combat}", "${Me.CombatState}",
            "${Me.AutoFire}", "${Me.Invis}", "${Me.Pet.ID}", "${Target.ID}",
            "${Stick.Active}", "${Raid.Members}", "${Raid.MainAssist[1].CleanName}"
        };
        private Dictionary<string, string> _workerState;
        private bool _commandsFailed;
        private bool _stopRequested;
        private bool _attack;
        private bool _stick;
        private bool _afollow;
        private bool _navigation;
        internal static string ConfigurationDirectory = "";
        internal static int BrokerPort = Comms.TcpProtocol.DefaultPort;
        internal static string TokenFile = "";
        private static string _serverLabel = "";
        private static bool _commsAllowed;
        private static string _importRoot = "";
        private static string _configRoot = "";

        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string Parse(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern void Command(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern void Print(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern int LendThread(int lend);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern int CommandExists(string name);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string ScriptsConfigDirectory();
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern int Bind(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern void Unbind();
        [MethodImpl(MethodImplOptions.InternalCall)] internal static extern string ConfigDirectory();

        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string PluginDirectory();
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern int NetworkAllowed();
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern int UseAbility(int kind, string name);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string AbilityRecords();
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern uint AAValue(int group);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string XTargets();
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string RaidHealMembers();
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern string ItemClick(int slot, int sub);
        [MethodImpl(MethodImplOptions.InternalCall)] private static extern byte[] Bulk(int kind);

        private static void LoadSettings()
        {
            var directory = PluginDirectory();
            if (string.IsNullOrEmpty(directory)) return;
            try
            {
                var own = Path.Combine(directory, "sentinel.json");
                var parent = Path.GetDirectoryName(directory);
                var config = ReadSettings(own);
                var root = Comms.WireJson.Text(config, "config_root");
                if (!string.IsNullOrWhiteSpace(root))
                    _configRoot = Path.GetFullPath(Path.Combine(directory, root));

                var settings = own;
                if (string.IsNullOrWhiteSpace(Comms.WireJson.Text(config, "token_file")) &&
                    !string.IsNullOrEmpty(parent))
                {
                    settings = Path.Combine(parent, "MQSentinel", "sentinel.json");
                    config = ReadSettings(settings);
                }
                _serverLabel = Comms.WireJson.Text(config, "server");
                _importRoot = Comms.WireJson.Text(config, "cs_config_dir");
                if (_importRoot.Length > 0)
                    _importRoot = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(settings), _importRoot));
                int port = Comms.WireJson.Int(config, "port", Comms.TcpProtocol.DefaultPort);
                if (port < 1 || port > 65535) return;
                string token = Comms.WireJson.Text(config, "token_file");
                if (string.IsNullOrWhiteSpace(token)) return;
                TokenFile = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(settings), token));
                BrokerPort = port;
            }
            catch (Exception ex) when (ex is IOException || ex is ArgumentException ||
                                       ex is FormatException || ex is UnauthorizedAccessException)
            {
                Print("[Sentinel] Could not read the HSMono settings.");
            }
        }

        private static Dictionary<string, object> ReadSettings(string path)
        {
            if (!File.Exists(path) || new FileInfo(path).Length > 65536)
                return new Dictionary<string, object>();
            return Comms.WireJson.Object(Comms.WireJson.Decode(File.ReadAllText(path)));
        }

        internal static bool SharesCharacterConfig { get { return _configRoot.Length != 0; } }

        internal static string ResolveConfigurationDirectory(string server, string character)
        {
            if (!SharesCharacterConfig) return ConfigurationDirectory;
            if (!PathSegment(server) || server.Equals("NULL", StringComparison.OrdinalIgnoreCase)) return "";
            return Config.CharConfig.ResolveCharacterConfigDirFromRoot(_configRoot, server, character);
        }

        private static bool PathSegment(string text)
        {
            return !string.IsNullOrWhiteSpace(text) && text != "." && text != ".." &&
                text.IndexOfAny(Path.GetInvalidFileNameChars()) < 0;
        }

        private static void ImportCharacterConfig()
        {
            if (ConfigurationDirectory.Length == 0) return;
            try
            {
                string character = Parse("${Me.CleanName}");
                string server = _serverLabel.Length != 0 ? _serverLabel : Parse("${EverQuest.Server}");
                if (!PathSegment(character) || !PathSegment(server)) return;
                string file = character + "_cs.lua";
                string destination = Path.Combine(ConfigurationDirectory, file);
                if (File.Exists(destination)) return;

                string root = _importRoot;
                if (root.Length == 0)
                {
                    string scriptsConfig = ScriptsConfigDirectory();
                    if (!string.IsNullOrEmpty(scriptsConfig))
                    {
                        string candidate = Path.Combine(scriptsConfig, "Sentinel");
                        if (Directory.Exists(candidate)) root = candidate;
                    }
                }
                if (root.Length == 0 && TokenFile.Length != 0)
                {
                    var directory = new FileInfo(TokenFile).Directory;
                    for (int i = 0; i < 10 && directory != null; ++i, directory = directory.Parent)
                    {
                        string candidate = Path.Combine(directory.FullName, "config", "Sentinel");
                        if (Directory.Exists(candidate)) { root = candidate; break; }
                    }
                }
                if (root.Length == 0) return;
                foreach (string label in new[] { server, server.Replace(" ", "") })
                {
                    string source = Path.Combine(root, label, file);
                    if (!File.Exists(source)) continue;
                    File.Copy(source, destination, false);
                    Print("[Sentinel] Imported the existing character settings into Hailstone.");
                    return;
                }
            }
            catch (Exception ex) when (ex is IOException || ex is ArgumentException ||
                                       ex is UnauthorizedAccessException)
            {
                Print("[Sentinel] Could not import the existing character settings.");
            }
        }

        private static void RefreshComms()
        {
            bool allowed = TokenFile.Length > 0 && NetworkAllowed() != 0;
            if (allowed == _commsAllowed) return;
            if (!allowed) Comms.CommsChannel.Shutdown();
            _commsAllowed = allowed;
            Comms.CommsChannel.Enabled = allowed;
        }

        public static void OnInit()
        {
            var adapter = new HailstoneHost();
            Core.Host = adapter;
            XTargetWindow.BulkSlots = adapter.ReadXTargets;
            AbilityCatalog.ReadItemClick = ReadItemClick;
            ClericBrain.ReadRaidHealMembers = adapter.ReadRaidHealMembers;
            ConfigurationDirectory = ConfigDirectory() ?? "";
            if (ConfigurationDirectory.Length != 0)
            {
                const string preferences = "sentinel-ui.json";
                string previous = Path.Combine(ConfigurationDirectory, "..", "HailstoneMono", preferences);
                WindowPreferences.Load(Path.Combine(ConfigurationDirectory, preferences), previous);
            }
            Comms.CommsChannel.Enabled = false;
            // Feed NativeData's per-pulse snapshot from the host's bulk
            // reads. Without this the capture below is a no-op, the
            // snapshot stays empty, and every spawn/buff/xtarget field
            // becomes an individual parsed query marshalled from the
            // worker - hundreds per turn, parked inside the frame.
            // Capture runs on the client thread, so both providers may
            // call the native host directly.
            Native.HailstoneNativeSource.Provider = Bulk;
            Native.HailstoneNativeSource.QueryProvider = q => Parse(q);
            _pulsed = false;
            LoadSettings();
            if (!SharesCharacterConfig) ImportCharacterConfig();
            RefreshComms();
            ClearAbilities();
            Core.OnInit();
            if (adapter._commandsFailed || !Core.IsProcessing)
            {
                OnStop();
                throw new InvalidOperationException("Sentinel commands are already owned by another plugin or Sentinel failed to start");
            }
        }

        // Hailstone pulses plugins once per rendered frame, and Core.OnPulse
        // parks that frame until the worker yields: unthrottled, the brain
        // turn rate equals the frame rate and every frame pays a full turn.
        // Sentinel's decisions, comms and command delivery are all built
        // around coarser timers (NativeData itself refreshes spawns every
        // 50 ms), so grant at most one turn per interval and let the other
        // frames render. Chat and slash commands only enqueue, so nothing
        // is lost between turns - worst case they wait one interval.
        private const int MinPulseIntervalMs = 50;
        private static int _lastPulseMs;
        private static bool _pulsed;

        public static void OnPulse()
        {
            RefreshComms();
            int now = Environment.TickCount;
            // Overflow-safe elapsed check; TickCount wraps every ~49.7 days.
            if (_pulsed && (int)((uint)now - (uint)_lastPulseMs) < MinPulseIntervalMs) return;
            _pulsed = true;
            _lastPulseMs = now;
            Core.OnPulse();
        }

        // Hailstone calls this from the plugin's OnUpdateImGui export, inside
        // the client's frame. Nothing may leave here: the call enters managed
        // code from native, and an exception would unwind into the client's
        // render pass.
        public static void OnUpdateImGui()
        {
            try { Core.OnUpdateImGui(); }
            catch (Exception ex) { Core.ReportUiError("hailstone UI draw", ex); }
        }

        public static void OnStop()
        {
            var adapter = (HailstoneHost)Core.Host;
            try { Core.OnStop(); }
            finally
            {
                XTargetWindow.BulkSlots = null;
                AbilityCatalog.ReadItemClick = null;
                ClericBrain.ReadRaidHealMembers = null;
                adapter._readReady.Dispose();
                if (adapter._attack) Command("/attack off");
                if (adapter._stick) Command("/stick off");
                if (adapter._afollow) Command("/afollow off");
                if (adapter._navigation) Command("/nav stop");
            }
        }
        public static void OnCommand(string text) { Core.OnCommand(text); }
        public static void OnIncomingChat(string text) { Core.OnIncomingChat(text); }
        public static string Status() { return ((HailstoneHost)Core.Host)._stopRequested ? "stop" : ""; }

        internal static bool WaitForWorker(WorkerBoundary boundary, int timeout)
        {
            var adapter = Core.Host as HailstoneHost;
            if (adapter == null) return boundary.WaitYield(timeout);
            // When the client supports lend_thread (ABI 4+), lend the client
            // thread while OnPulse waits so the worker can call host reads
            // directly without cross-thread queue marshalling. Older clients
            // return 0 and keep the marshal loop below.
            int lent = LendThread(1);
            if (lent != 0)
            {
                adapter._threadLent = true;
                try { return boundary.WaitYield(timeout); }
                finally
                {
                    adapter._threadLent = false;
                    if (lent == 1) LendThread(0);
                }
            }
            var clock = Stopwatch.StartNew();
            var signals = new[] { boundary.YieldSignal, adapter._readReady };
            do
            {
                Read read;
                while (adapter._reads.TryDequeue(out read))
                {
                    try { read.Value = read.Fetch(); }
                    catch (Exception ex) { read.Error = ExceptionDispatchInfo.Capture(ex); }
                    finally { read.Done.Set(); }
                    if (clock.ElapsedMilliseconds >= timeout) return false;
                }
                int remaining = timeout - (int)clock.ElapsedMilliseconds;
                if (remaining <= 0) break;
                int ready = WaitHandle.WaitAny(signals, remaining);
                if (ready == 0) return true;
                if (ready == WaitHandle.WaitTimeout) break;
            } while (clock.ElapsedMilliseconds < timeout);
            return false;
        }

        internal bool TryReadWorkerState(string expression, out string value)
        {
            // Core has already checked the worker's grant. A detached
            // continuation sees the new snapshot on the next grant.
            value = null;
            return Thread.CurrentThread.ManagedThreadId != _clientThread &&
                _workerState != null && _workerState.TryGetValue(expression, out value);
        }

        string IGameHost.Query(string expression) { return ReadOnClient(() => QueryClient(expression)); }

        bool? IGameHost.CommandExists(string command)
        {
            if (string.IsNullOrWhiteSpace(command)) return false;
            string result = ReadOnClient(() => CommandExists(command).ToString(CultureInfo.InvariantCulture));
            int code;
            if (!int.TryParse(result, NumberStyles.Integer, CultureInfo.InvariantCulture, out code) || code < 0)
                return null;
            return code != 0;
        }

        private string ReadOnClient(Func<string> fetch)
        {
            if (Thread.CurrentThread.ManagedThreadId == _clientThread || _threadLent)
                return fetch();
            var read = new Read { Fetch = fetch };
            _reads.Enqueue(read);
            _readReady.Set();
            read.Done.Wait();
            // The client may still be returning from Set; the event is
            // collected with the request instead of being disposed here.
            if (read.Error != null) read.Error.Throw();
            return read.Value ?? "NULL";
        }

        private static bool Indexed(string text, string start, string end, out string value)
        {
            value = "";
            if (text.Length <= start.Length + end.Length ||
                !text.StartsWith(start, StringComparison.OrdinalIgnoreCase) ||
                !text.EndsWith(end, StringComparison.Ordinal)) return false;
            value = text.Substring(start.Length, text.Length - start.Length - end.Length);
            return value.IndexOfAny(new[] { '[', ']', '{', '}', '\r', '\n' }) < 0;
        }

        private sealed class Ability
        {
            internal int Kind;
            internal int Id;
            internal string Name;
        }

        private static readonly object AbilityGate = new object();
        private static List<Ability> _abilities;

        // The Hailstone host lists the Ability Book's entries in one bulk read.
        // Keep the small map for the picker and for MQ's ${Me.AltAbility[...]}
        // expressions, which Hailstone has no TLO type for.
        private static List<Ability> Abilities()
        {
            lock (AbilityGate)
            {
                if (_abilities != null) return _abilities;
                var list = new List<Ability>();
                string text = AbilityRecords() ?? "";
                foreach (string line in text.Split('\n'))
                {
                    if (line.Length == 0) continue;
                    string[] parts = line.Split('\t');
                    int kind, id;
                    if (parts.Length != 3
                        || !int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out kind)
                        || !int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out id)
                        || id <= 0 || parts[2].Length == 0) continue;
                    list.Add(new Ability { Kind = kind, Id = id, Name = parts[2] });
                }
                _abilities = list.Count > 0 ? list : null;
                return list;
            }
        }

        private static void ClearAbilities()
        {
            lock (AbilityGate) _abilities = null;
        }

        // field: 0 name, 1 id, 2 owned rank.
        private static string AbilityQuery(string key, int field)
        {
            int id = 0;
            string name = "";
            if (int.TryParse(key, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0)
            {
                foreach (Ability ability in Abilities())
                    if (ability.Kind == HS_ABILITY_ALT && ability.Id == id) { name = ability.Name; break; }
            }
            else
            {
                foreach (Ability ability in Abilities())
                    if (ability.Kind == HS_ABILITY_ALT
                        && string.Equals(ability.Name, key, StringComparison.OrdinalIgnoreCase))
                    { id = ability.Id; name = ability.Name; break; }
            }
            if (id <= 0) return "NULL";
            if (field == 0) return string.IsNullOrEmpty(name) ? "NULL" : name;
            if (field == 1) return id.ToString(CultureInfo.InvariantCulture);
            return AAValue(id).ToString(CultureInfo.InvariantCulture);
        }

        private static string QueryClient(string expression)
        {
            if (expression == "${EverQuest.Server}" && _serverLabel.Length != 0) return _serverLabel;
            string ability;
            if (Indexed(expression, "${Me.AltAbility[", "].Name}", out ability)) return AbilityQuery(ability, 0);
            if (Indexed(expression, "${Me.AltAbility[", "].ID}", out ability)) return AbilityQuery(ability, 1);
            if (Indexed(expression, "${Me.AltAbility[", "].Rank}", out ability)) return AbilityQuery(ability, 2);
            string value = Parse(expression);
            if (!string.IsNullOrEmpty(value) && value != "NULL") return value;
            string spell;
            if (Indexed(expression, "${Cast.Ready[", "]}", out spell))
            {
                long remaining;
                return Parse("${Me.SpellReady[" + spell + "]}") == "TRUE" &&
                    long.TryParse(Parse("${Me.GemTimer[" + spell + "]}"), out remaining) && remaining == 0
                    ? "TRUE" : "FALSE";
            }
            return value;
        }

        internal static string ResolveCommand(string text, Func<string, string> parse)
        {
            string name;
            int slot;
            if (Indexed(text, "/cast \"=", "\"", out name))
            {
                return int.TryParse(parse("${Me.Gem[" + name + "]}"), NumberStyles.None,
                                    CultureInfo.InvariantCulture, out slot) && slot >= 1 && slot <= 12
                    ? "/cast " + slot : null;
            }
            if (Indexed(text, "/useitem \"", "\"", out name))
            {
                string query = "${FindItem[=" + name + "]";
                if (!int.TryParse(parse(query + ".ItemSlot}"), out slot))
                {
                    query = "${FindItem[" + name + "]";
                    if (!int.TryParse(parse(query + ".ItemSlot}"), out slot)) return null;
                }
                if (slot < 0 || slot > 32) return null;
                int sub;
                if (!int.TryParse(parse(query + ".ItemSlot2}"), out sub) || sub < -1) return null;
                return sub >= 0 ? "/useitem " + slot + " " + sub : "/useitem " + slot;
            }
            if (Indexed(text, "/itemnotify ", " rightmouseup", out name))
            {
                if (string.Equals(name, "chest", StringComparison.OrdinalIgnoreCase))
                    name = "17"; // RoF2's chest possessions slot
                return int.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out slot) &&
                    slot >= 0 && slot <= 32 ? "/useitem " + slot : null;
            }
            string[] words = text.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            if (words.Length > 1 && words[0].Equals("/stick", StringComparison.OrdinalIgnoreCase))
            {
                var supported = new List<string> { words[0] };
                for (int i = 1; i < words.Length; i++)
                {
                    switch (words[i].ToLowerInvariant())
                    {
                        case "front":
                        case "!front":
                        case "pin":
                            break;
                        case "behindonce":
                            supported.Add("behind");
                            break;
                        default:
                            supported.Add(words[i]);
                            break;
                    }
                }
                return string.Join(" ", supported);
            }
            return text;
        }

        private readonly HashSet<string> _reportedCommandFailures = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        private void ReportCommandFailure(string text)
        {
            if (_reportedCommandFailures.Count < 128 && _reportedCommandFailures.Add(text))
                Print("[Sentinel/HSMono] Hailstone could not execute " + text);
        }

        void IGameHost.Execute(string text)
        {
            const string aa = "/alt activate ", aaShort = "/alt act ", disc = "/disc ";
            if (text.StartsWith(aa, StringComparison.OrdinalIgnoreCase) ||
                text.StartsWith(aaShort, StringComparison.OrdinalIgnoreCase))
            {
                int offset = text.StartsWith(aa, StringComparison.OrdinalIgnoreCase) ? aa.Length : aaShort.Length;
                int id;
                if (!int.TryParse(text.Substring(offset), NumberStyles.None, CultureInfo.InvariantCulture, out id)
                    || id <= 0 || UseAbility(2, id.ToString(CultureInfo.InvariantCulture)) == 0)
                    ReportCommandFailure(text);
                return;
            }
            if (text.StartsWith(disc, StringComparison.OrdinalIgnoreCase))
            {
                if (UseAbility(1, text.Substring(disc.Length)) == 0) ReportCommandFailure(text);
                return;
            }
            string skill;
            if (Indexed(text, "/doability \"", "\"", out skill))
            {
                bool knownSkill = Parse("${Me.Ability[" + skill + "]}") == "TRUE";
                if (UseAbility(knownSkill ? 0 : 1, skill) == 0) ReportCommandFailure(text);
                return;
            }
            var line = ResolveCommand(text, Parse);
            if (line == null) { ReportCommandFailure(text); return; }
            Command(line);
            if (text.Equals("/attack on", StringComparison.OrdinalIgnoreCase))
                _attack = Parse("${Me.Combat}") == "TRUE";
            else if (text.Equals("/attack off", StringComparison.OrdinalIgnoreCase)) _attack = false;
            else if (text.StartsWith("/stick ", StringComparison.OrdinalIgnoreCase))
                _stick = Parse("${Stick.Active}") == "TRUE";
            else if (text.StartsWith("/afollow", StringComparison.OrdinalIgnoreCase))
                _afollow = Parse("${AdvPath.Following}") == "TRUE";
            else if (text.StartsWith("/nav ", StringComparison.OrdinalIgnoreCase))
                _navigation = Parse("${Navigation.Active}") == "TRUE";
        }
        void IGameHost.ExecuteDeferred(string text)
        {
            if (text.Equals("/mono unload Sentinel", StringComparison.OrdinalIgnoreCase)) _stopRequested = true;
            else ((IGameHost)this).Execute(text);
        }
        void IGameHost.Echo(string text) { Print(text); }
        void IGameHost.RegisterCommand(string command)
        {
            // In MacroQuest, MQSentinel's native RmlUi plugin owned the window
            // commands and the C# registered only the behaviour commands. In
            // Hailstone the C# ImGui windows are the only implementation, so
            // /sentpeers, /sentstuck, /sentoptions, /sentmini and the rest must
            // be bound here too.
            if (Bind(command) == 0) _commandsFailed = true;
        }
        void IGameHost.ClearCommands() { Unbind(); }
        void IGameHost.InitializeWindows()
        {
            // Hailstone has no MQSentinel native/RmlUi plugin. Clear the
            // fallback flags so HeartbeatWindow, AAPeerWindow, XTarget and ToT
            // draw their own ImGui windows instead of believing a native
            // roster owns them.
            Core.GuardUi("native UI disconnect", Sentinel.NativeUiBridge.Disconnect);

            // Each OnInit only touches the native open-flag map (and one query),
            // so it is safe from the command callback that starts the session.
            // One guard per window keeps a broken window from taking the rest.
            Core.GuardUi("init XTarget", XTargetWindow.OnInit);
            Core.GuardUi("init Heartbeat", HeartbeatWindow.OnInit);
            Core.GuardUi("init TcpStats", TcpStatsWindow.OnInit);
            Core.GuardUi("init AAPeers", AAPeerWindow.OnInit);
            Core.GuardUi("init CommandHelp", CommandHelpWindow.OnInit);
            Core.GuardUi("init Songs", SongWindow.OnInit);
            Core.GuardUi("init ToT", TargetsTargetWindow.OnInit);
            Core.GuardUi("init Options", OptionsWindow.OnInit);
            Core.GuardUi("init MiniHUD", MiniHud.OnInit);
            Core.GuardUi("init Stuck", StuckWindow.OnInit);
            Core.GuardUi("init Debug", DebugWindow.OnInit);

            // Hailstone window defaults. The client tracks the HoTT id
            // internally (GameConnection::target_of_target) but exposes no
            // TargetOfTarget TLO or plugin accessor for it yet, so the ToT
            // card can only ever read "Target has no target" here: keep it
            // hidden until that data is reachable (/senttt still opens
            // it). The peers window is the
            // opposite: with no MQSentinel native roster on this host, the
            // managed window is the only peer view, so show it by default
            // (/sentpeers still toggles it).
            Core.GuardUi("default ToT closed", () => TargetsTargetWindow.SetOpen(false));
            Core.GuardUi("default peers open", () => HeartbeatWindow.SetOpen(true));
        }

        private const int MiniLogoSize = 38;
        private bool _miniLogoLoaded;
        private uint[] _miniLogo;

        private static uint[] ReadMiniLogo(string path)
        {
            using (var source = new Bitmap(path))
            using (var bitmap = new Bitmap(MiniLogoSize, MiniLogoSize))
            {
                using (var graphics = Graphics.FromImage(bitmap))
                {
                    graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    graphics.DrawImage(source, 0, 0, MiniLogoSize, MiniLogoSize);
                }

                var pixels = new uint[MiniLogoSize * MiniLogoSize];
                for (int y = 0; y < MiniLogoSize; ++y)
                for (int x = 0; x < MiniLogoSize; ++x)
                {
                    Color pixel = bitmap.GetPixel(x, y);
                    pixels[y * MiniLogoSize + x] = (uint)pixel.A << 24
                        | (uint)pixel.B << 16 | (uint)pixel.G << 8 | pixel.R;
                }
                return pixels;
            }
        }

        private bool? DrawMiniLogo()
        {
            if (!_miniLogoLoaded)
            {
                _miniLogoLoaded = true;
                string directory = Path.GetDirectoryName(typeof(HailstoneHost).Assembly.Location);
                string path = Path.Combine(directory, "resources", "sentinel_cat_128.png");
                if (File.Exists(path))
                {
                    try { _miniLogo = ReadMiniLogo(path); }
                    catch (Exception ex) { Core.QueueWrite("[Sentinel/mini] " + path + ": " + ex.Message); }
                }
            }
            if (_miniLogo == null) return null;

            // HSMono has no file-backed ImGui textures. Draw the cached
            // button-sized logo with the same draw list as the HUD toggles.
            bool clicked = imgui_InvisibleButton("SentinelIcon", MiniLogoSize, MiniLogoSize, 0);
            float left = imgui_GetItemRectMinX();
            float top = imgui_GetItemRectMinY();
            for (int y = 0; y < MiniLogoSize; ++y)
            for (int x = 0; x < MiniLogoSize; ++x)
            {
                uint pixel = _miniLogo[y * MiniLogoSize + x];
                if (pixel >> 24 != 0)
                    imgui_GetWindowDrawList_AddRectFilled(
                        left + x, top + y, left + x + 1, top + y + 1, pixel, 0f, 0);
            }
            return clicked;
        }

        void IGameHost.RenderWindows()
        {
            Core.GuardUi("XTarget", XTargetWindow.OnUpdateImGui);
            Core.GuardUi("Heartbeat", HeartbeatWindow.OnUpdateImGui);
            Core.GuardUi("TcpStats", TcpStatsWindow.OnUpdateImGui);
            Core.GuardUi("AAPeers", AAPeerWindow.OnUpdateImGui);
            Core.GuardUi("CommandHelp", CommandHelpWindow.OnUpdateImGui);
            Core.GuardUi("Songs", SongWindow.OnUpdateImGui);
            Core.GuardUi("ToT", TargetsTargetWindow.OnUpdateImGui);
            Core.GuardUi("Options", OptionsWindow.OnUpdateImGui);
            Core.GuardUi("Picker", AbilityPicker.OnUpdateImGui);
            Core.GuardUi("MiniHUD", () => MiniHud.OnUpdateImGui(DrawMiniLogo));
            Core.GuardUi("Stuck", StuckWindow.OnUpdateImGui);
            Core.GuardUi("Debug", DebugWindow.OnUpdateImGui);
            Core.GuardUi("EncounterAlert", Encounters.EncounterAlertWindow.OnUpdateImGui);
        }
        void IGameHost.CaptureNativeData()
        {
            _workerState = null;
            Native.NativeData.Capture(Native.HailstoneNativeSource.Instance);
            var state = new Dictionary<string, string>(WorkerStateFields.Length, StringComparer.Ordinal);
            foreach (string field in WorkerStateFields)
                state.Add(field, QueryClient(field) ?? "NULL");
            _workerState = state;
        }
        void IGameHost.StageSpawn() { }
        void IGameHost.SetSpawns() { }
        private static int[] ReadAbilityIds(int kind)
        {
            var ids = new List<int>();
            foreach (Ability ability in Abilities())
                if (ability.Kind == kind) ids.Add(ability.Id);
            return ids.ToArray();
        }

        private List<ClericBrain.RaidHealMember> ReadRaidHealMembers()
        {
            string text = ReadOnClient(RaidHealMembers);
            if (text == "NULL") return null;
            var members = new List<ClericBrain.RaidHealMember>();
            foreach (string line in text.Split('\n'))
            {
                string[] parts = line.Split('\t');
                int id, hp;
                if (parts.Length != 3
                    || !int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out id)
                    || !int.TryParse(parts[1], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out hp))
                    continue;
                members.Add(new ClericBrain.RaidHealMember { Id = id, Hp = hp, Name = parts[2] });
            }
            return members;
        }

        private static AbilityCatalog.ItemClick ReadItemClick(int slot, int sub)
        {
            string text = ItemClick(slot, sub);
            if (string.IsNullOrEmpty(text)) return new AbilityCatalog.ItemClick();
            int separator = text.IndexOf('\t');
            int spell;
            if (separator <= 0 || !int.TryParse(text.Substring(0, separator), NumberStyles.None,
                                                CultureInfo.InvariantCulture, out spell))
                return new AbilityCatalog.ItemClick();
            return new AbilityCatalog.ItemClick { SpellId = spell, Name = text.Substring(separator + 1) };
        }

        // Null means the running client has no xtarget bulk accessor, so the
        // caller may use its TLO fallback. An empty list is a valid "no
        // extended targets" answer.
        private List<XTargetWindow.Slot> ReadXTargets()
        {
            string text = ReadOnClient(XTargets);
            if (text == null || text == "NULL") return null;
            var slots = new List<XTargetWindow.Slot>();
            foreach (string line in text.Split('\n'))
            {
                if (line.Length == 0) continue;
                string[] parts = line.Split('\t');
                if (parts.Length != 10) continue;
                int index, id, aggro, hp, level, con, distanceTenths, valid, role;
                if (!int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out index)
                    || !int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out id)
                    || !int.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out aggro)
                    || !int.TryParse(parts[3], NumberStyles.None, CultureInfo.InvariantCulture, out hp)
                    || !int.TryParse(parts[4], NumberStyles.None, CultureInfo.InvariantCulture, out level)
                    || !int.TryParse(parts[5], NumberStyles.None, CultureInfo.InvariantCulture, out con)
                    || !int.TryParse(parts[7], NumberStyles.None, CultureInfo.InvariantCulture, out distanceTenths)
                    || !int.TryParse(parts[8], NumberStyles.None, CultureInfo.InvariantCulture, out valid)
                    || !int.TryParse(parts[9], NumberStyles.None, CultureInfo.InvariantCulture, out role)
                    || index <= 0) continue;
                slots.Add(new XTargetWindow.Slot
                {
                    Index = index,
                    Id = id,
                    AutoHater = role == 1, // HS_BULK_XTARGET_AUTO_HATER
                    Aggro = aggro,
                    Hp = hp,
                    Level = level,
                    ConColor = valid != 0 ? XTargetWindow.ConName(con) : "",
                    Name = parts[6],
                    Distance = distanceTenths / 10f,
                    Valid = valid != 0,
                });
            }
            return slots;
        }
        int[] IGameHost.AvailableAAs() { return ReadAbilityIds(HS_ABILITY_ALT); }
        int[] IGameHost.AvailableDiscs() { return ReadAbilityIds(HS_ABILITY_DISCIPLINE); }
        List<Native.CapabilityRecord> IGameHost.CapabilityInfo() { return null; }
        string[] IGameHost.SpellEffects(string spell) { return new string[0]; }
    }
}
