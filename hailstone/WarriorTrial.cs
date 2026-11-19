// Compiled alongside the existing Sentinel sources by Build-WarriorTrial.ps1.
using System;
using System.Collections.Generic;
using MonoCore;
using System.Threading.Tasks;

namespace Sentinel
{
    public static class HailstoneWarriorTrial
    {
        private static SnapshotHost _snapshot;
        private static Task<string> _turn;

        public static void OnInit()
        {
            _snapshot = new SnapshotHost();
            Core.Host = _snapshot;
            Core.IsProcessing = true;
            DecisionHistory.SetEnabled(true);
            WarriorBrain.AutomationEnabled = false;
            WarriorBrain.SmartTaunt = false;
        }

        public static string OnPulse(string snapshot)
        {
            if (_turn != null) return "Wait for the current warrior decision.";
            _turn = Task.Run(() => Decide(snapshot));
            return "Sentinel warrior snapshot submitted.";
        }

        public static string OnPoll()
        {
            if (_turn == null || !_turn.IsCompleted) return null;
            var completed = _turn;
            _turn = null;
            try { return completed.GetAwaiter().GetResult(); }
            catch (Exception ex) { return "Sentinel warrior decision failed: " + ex.GetType().Name + ": " + ex.Message; }
        }

        private static string Decide(string snapshot)
        {
            string[] lines = snapshot.Split('\n');
            _snapshot.Load(lines);
            QueryReuse.Reset();
            DecisionHistory.Clear();
            DecisionHistory.BeginPulse();
            Core.DiscardPendingGameCommands();
            if (lines[0] == "disengage")
            {
                Targeting.StopCombat();
                Targeting.EngageTargetId = 0;
                WarriorBrain.Disengage("Hailstone dry run");
            }
            else
            {
                if (Core.QueryString("${Me.Class.ShortName}", "") != "WAR")
                    return "Select a warrior character for this trial.";
                int id = Core.QueryInt("${Target.ID}", 0);
                if (!Targeting.IsEngageableSpawn(id) || !Targeting.InRange(id))
                    return "Select a living NPC, pet, or object within engage range.";
                Targeting.ResumeCombat();
                Targeting.EngageTargetId = id;
                WarriorBrain.Engage(id);
                WarriorBrain.Tick(Comms.CommsChannel.NowMs());
            }

            var result = new List<string>();
            bool attack = false;
            string attackTrace = " /attack on kind=AttackOn target=" + Core.QueryInt("${Target.ID}", 0) + " ";
            foreach (string entry in DecisionHistory.Recent(DecisionHistory.Capacity))
            {
                if (entry.Contains(" queued ") && entry.Contains(attackTrace)) attack = true;
                if (entry.Contains(" queued ") || entry.Contains(" rejected ")) result.Add(entry);
            }
            if (result.Count == 0) result.Add("Warrior proposed no actions.");
            if (_snapshot.Missing.Count != 0)
                result.Add("Uncaptured reads: " + string.Join(", ", _snapshot.Missing));
            Core.DiscardPendingGameCommands();
            // Mark only the exact built-in attack action proposed for this snapshot.
            // The native side authorizes and revalidates any delivery.
            return (lines[0] == "attack" && attack ? "ATTACK_ON\n" : "") + string.Join("\n", result);
        }

        public static void OnStop()
        {
            var pending = _turn;
            _turn = null;
            if (pending != null)
            {
                try { pending.GetAwaiter().GetResult(); }
                catch (Exception) { /* A failed dry run has no game side effects. */ }
            }
            Core.DiscardPendingGameCommands();
            Core.IsProcessing = false;
            DecisionHistory.SetEnabled(false);
        }

        private sealed class SnapshotHost : IGameHost
        {
            private readonly Dictionary<string, string> _values =
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            internal readonly SortedSet<string> Missing = new SortedSet<string>(StringComparer.Ordinal);

            internal void Load(string[] lines)
            {
                Missing.Clear();
                _values.Clear();
                for (int i = 1; i < lines.Length; i++)
                {
                    int separator = lines[i].IndexOf('\t');
                    if (separator >= 0)
                        _values.Add(lines[i].Substring(0, separator), lines[i].Substring(separator + 1));
                }
            }

            public string Query(string expression)
            {
                string value;
                if (_values.TryGetValue(expression, out value)) return value;
                Missing.Add(expression);
                return "NULL";
            }
            public void Execute(string command) { throw new InvalidOperationException("Dry-run command delivery"); }
            public void ExecuteDeferred(string command) { Execute(command); }
            public void Echo(string text) { }
            public void RegisterCommand(string command) { }
            public void ClearCommands() { }
            public void InitializeWindows() { }
            public void RenderWindows() { }
            public void CaptureNativeData() { }
            public void StageSpawn() { }
            public void SetSpawns() { }
            public int[] AvailableAAs() { return new int[0]; }
            public int[] AvailableDiscs() { return new int[0]; }
            public List<Native.CapabilityRecord> CapabilityInfo() { return null; }
            public string[] SpellEffects(string spell) { return new string[0]; }
        }
    }
}
