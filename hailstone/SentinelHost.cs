using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;
using MonoCore;

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
            internal readonly ManualResetEventSlim Done = new ManualResetEventSlim();
        }

        private readonly ConcurrentQueue<Read> _reads = new ConcurrentQueue<Read>();
        private readonly AutoResetEvent _readReady = new AutoResetEvent(false);
        private readonly int _clientThread = Thread.CurrentThread.ManagedThreadId;
        private bool _commandsFailed;
        private bool _stopRequested;
        private bool _attack;
        private bool _stick;
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

        public static void OnPulse() { RefreshComms(); Core.OnPulse(); }

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
            var clock = Stopwatch.StartNew();
            var signals = new[] { boundary.YieldSignal, adapter._readReady };
            do
            {
                Read read;
                while (adapter._reads.TryDequeue(out read))
                {
                    try { read.Value = read.Fetch(); }
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

        string IGameHost.Query(string expression) { return ReadOnClient(() => QueryClient(expression)); }

        private string ReadOnClient(Func<string> fetch)
        {
            if (Thread.CurrentThread.ManagedThreadId == _clientThread)
                return fetch();
            var read = new Read { Fetch = fetch };
            _reads.Enqueue(read);
            _readReady.Set();
            read.Done.Wait();
            // The client may still be returning from Set; the event is
            // collected with the request instead of being disposed here.
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
                return Parse("${Me.SpellReady[" + spell + "]}");
            if (Indexed(expression, "${Me.GemTimer[", "]}", out spell))
                return Parse("${Me.SpellReady[" + spell + "]}") == "TRUE" ? "0" : "NULL";
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
                return int.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out slot) &&
                    slot >= 0 && slot <= 32 ? "/useitem " + slot : null;
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

        void IGameHost.Execute(string text)
        {
            const string aa = "/alt activate ";
            if (text.StartsWith(aa, StringComparison.OrdinalIgnoreCase))
            {
                int id;
                if (int.TryParse(text.Substring(aa.Length), NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0)
                    UseAbility(2, id.ToString(CultureInfo.InvariantCulture));
                return;
            }
            string skill;
            if (Indexed(text, "/doability \"", "\"", out skill))
            {
                UseAbility(0, skill);
                return;
            }
            var line = ResolveCommand(text, Parse);
            if (line == null) return;
            Command(line);
            if (text.Equals("/attack on", StringComparison.OrdinalIgnoreCase))
                _attack = Parse("${Me.Combat}") == "TRUE";
            else if (text.Equals("/attack off", StringComparison.OrdinalIgnoreCase)) _attack = false;
            else if (text.StartsWith("/stick ", StringComparison.OrdinalIgnoreCase))
                _stick = Parse("${Stick.Active}") == "TRUE";
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
            Core.GuardUi("MiniHUD", MiniHud.OnUpdateImGui);
            Core.GuardUi("Stuck", StuckWindow.OnUpdateImGui);
            Core.GuardUi("Debug", DebugWindow.OnUpdateImGui);
            Core.GuardUi("EncounterAlert", Encounters.EncounterAlertWindow.OnUpdateImGui);
        }
        void IGameHost.CaptureNativeData() { }
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
