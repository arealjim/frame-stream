-- SPDX-License-Identifier: MIT
-- pad-trace.lua - a UEVR script that logs the Xbox pad state the game receives (after UEVR has turned the VR
-- controllers into it), one line per change of buttons or triggers: "pad-trace btn=0x.... LT RT A B ...".
-- With OXRSys's OXRSYS_ACTION_TRACE (what UEVR read) it closes the loop from a controller press to the game.
-- Goes in UnrealVRMod/<game exe stem>/scripts/.
local names = {
  {0x0001, "DUP"}, {0x0002, "DDOWN"}, {0x0004, "DLEFT"}, {0x0008, "DRIGHT"}, {0x0010, "START"}, {0x0020, "BACK"},
  {0x0040, "LS"}, {0x0080, "RS"}, {0x0100, "LB"}, {0x0200, "RB"}, {0x1000, "A"}, {0x2000, "B"}, {0x4000, "X"},
  {0x8000, "Y"},
}
-- UEVR's own log.txt; a plain print() only reaches stdout
local function log(msg) uevr.params.functions.log_info(msg) end
local last = nil
uevr.sdk.callbacks.on_xinput_get_state(function(retval, user_index, state)
  local g = state.Gamepad
  -- UEVR fills the pad only while it counts the controllers as in use (headset active, controllers seen, a
  -- recent press), so log that too: an empty pad with using=false is UEVR standing down, not a wrong layout
  local vr = uevr.params.vr
  local key = string.format("btn=0x%04x lt=%d rt=%d hmd=%s using=%s", g.wButtons, g.bLeftTrigger, g.bRightTrigger,
    tostring(vr.is_hmd_active()), tostring(vr.is_using_controllers()))
  if key == last then return end
  last = key
  local held = {}
  for _, n in ipairs(names) do
    if (g.wButtons & n[1]) ~= 0 then held[#held + 1] = n[2] end
  end
  if g.bLeftTrigger > 30 then held[#held + 1] = "LT" end
  if g.bRightTrigger > 30 then held[#held + 1] = "RT" end
  log("pad-trace user=" .. user_index .. " " .. key .. " " .. table.concat(held, " "))
end)
log("pad-trace: loaded")
