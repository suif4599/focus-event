# Pure Nix helpers for rendering structured focus-event rules into a KDL
# config file that the C++ binary can parse.
#
# The schema mirrors what the C++ side understands (see src/config.cpp).
# Selectors are AND-combined within a rule; multiple spawn actions are allowed.

{ lib }:

let
  inherit (lib) optional optionalString concatStringsSep concatMapStringsSep
    boolToString isBool isString replaceStrings escape;

  # Escape a string for safe embedding inside a KDL double-quoted string.
  # We escape backslash and double-quote; everything else is passed through.
  escapeKDL = s:
    let
      escaped = replaceStrings [ "\\" "\"" ] [ "\\\\" "\\\"" ] s;
    in ''"${escaped}"'';

  # Render a single scalar value (string | int | bool) the way KDL expects.
  renderScalar = v:
    if isBool v then boolToString v
    else if isString v then escapeKDL v
    else builtins.toString v;

  # Render one spawn action. argv is a list of strings.
  renderSpawnAction = argv:
    "    spawn " + concatMapStringsSep " " escapeKDL argv;

  # Render the optional selectors of a single rule as a space-separated string
  # of `key=value` pairs, skipping null entries.
  renderSelectors = rule:
    let
      pairs = (
        optional (rule.app-id or null != null)
          "app-id=${escapeKDL rule.app-id}"
        ++ optional (rule.title or null != null)
          "title=${escapeKDL rule.title}"
        ++ optional (rule.id or null != null)
          "id=${toString rule.id}"
        ++ optional (rule.workspace-id or null != null)
          "workspace-id=${toString rule.workspace-id}"
        ++ optional (rule.is-floating or null != null)
          "is-floating=${boolToString rule.is-floating}"
        ++ optional (rule.is-urgent or null != null)
          "is-urgent=${boolToString rule.is-urgent}"
      );
    in concatStringsSep " " pairs;

  renderRule = rule:
    let
      trigger = if rule.trigger == "focus" then "on-focus" else "on-blur";
      sels = renderSelectors rule;
      actions = concatStringsSep "\n" (map renderSpawnAction rule.spawn);
      header = if sels == "" then trigger else "${trigger} ${sels}";
    in ''
      ${header} {
      ${actions}
      }'';

  # Schema for one rule, exposed so the modules can use submodule types.
  ruleSubmodule = { lib }: { options = {
    trigger = lib.mkOption {
      type = lib.types.enum [ "focus" "blur" ];
      description = "When to fire this rule.";
    };
    "app-id" = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Regex matched against the window's app_id (substring match).";
    };
    title = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Regex matched against the window's title (substring match).";
    };
    id = lib.mkOption {
      type = lib.types.nullOr lib.types.ints.unsigned;
      default = null;
      description = "Exact numeric window id to match.";
    };
    "workspace-id" = lib.mkOption {
      type = lib.types.nullOr lib.types.ints.unsigned;
      default = null;
      description = "Match windows on this workspace.";
    };
    "is-floating" = lib.mkOption {
      type = lib.types.nullOr lib.types.bool;
      default = null;
      description = "Match floating (true) or tiled (false) windows.";
    };
    "is-urgent" = lib.mkOption {
      type = lib.types.nullOr lib.types.bool;
      default = null;
      description = "Match urgent (true) or non-urgent (false) windows.";
    };
    spawn = lib.mkOption {
      type = lib.types.listOf (lib.types.listOf lib.types.str);
      default = [ ];
      description = ''
        Spawn actions to fire when the rule matches. Each element is a list of
        argv strings (program + arguments). Multiple actions fire in order.
      '';
      example = lib.literalExpression ''
        [ [ "pactl" "set-sink-mute" "@DEFAULT_SINK@" "1" ] ];
      '';
    };
  }; };

in
{
  inherit ruleSubmodule;

  # Render a config attrset ({ rules = [...]; extraConfig = "..."; }) to KDL text.
  renderConfig = cfg:
    (concatStringsSep "\n" (map renderRule cfg.rules))
    + optionalString (cfg.extraConfig != "") ("\n" + cfg.extraConfig + "\n");
}
