using System;

namespace HSMono
{
    public static class Smoke
    {
        private static bool _started;

        public static void OnInit()
        {
            _started = true;
        }

        public static void OnPulse(string playerId)
        {
            uint id;
            if (!_started || !UInt32.TryParse(playerId, out id) || id == 0)
                throw new InvalidOperationException("Expected an active character snapshot");
        }

        public static void OnStop()
        {
            if (!_started)
                throw new InvalidOperationException("Smoke assembly did not start");
            _started = false;
        }
    }
}
