# 04 — Commands and undo

Every state change the user can trigger is a `Command`. Commands are the unit
of undo, the unit of engine synchronisation, and the unit of testing.

## Interface

```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual std::string label() const = 0;           // "Move clip", shown in Edit menu
    virtual bool apply(Model&) = 0;                   // false = refused; nothing changed
    virtual void revert(Model&) = 0;                  // must exactly undo a successful apply
    virtual bool mergeWith(const Command& next) { return false; }  // coalescing (see below)
    virtual std::optional<CommandRecord> record() const;  // for the edit journal (doc 09)
};
```

Rules:

- `apply` is **atomic**: it validates everything first, then mutates. If it
  returns `false` the model is untouched. Composite commands achieve this by
  running inside a `Transaction` that rolls back on failure.
- Commands capture what they need to revert **at apply time** (e.g. `MoveClip`
  records the old track/position when applied, not when constructed). This is
  why commands are objects, not closure pairs: the data is inspectable and
  serialisable for the journal and for test fixtures.
- Commands hold ids, never pointers or indices into model containers.
- `revert` after `apply` must restore an *equal* model
  (`Model::operator==` on the project value, ignoring nothing). This is a
  property test, see doc 11.

## Primitive commands (one mutator each)

| Command | Reverts by |
|---------|-----------|
| `InsertClip{track, asset, pos, in, out}` → sets `clipId` on apply | `removeClip(clipId)` |
| `RemoveClip{clipId}` (captures full `Clip` copy) | re-inserting with `reuseId` |
| `MoveClip{clipId, track, pos}` | moving back |
| `ResizeClip{clipId, in, out, pos}` | restoring old triple |
| `SplitClip{clipId, at}` → produces `rightId` | `JoinClips` inverse (only valid immediately; implemented as remove right + resize left) |
| `AddTrack{kind, index, name}` / `RemoveTrack{trackId}` (captures track + its clips) | inverse |
| `SetTrackFlags{trackId, muted, hidden, locked}` | old flags |
| `AddEffect{target, service, index}` / `RemoveEffect{target, effectId}` / `MoveEffect` | inverse |
| `SetParam{target, effectId, name, value}` | old value; **mergeable** |
| `SetKeyframe{…}` / `RemoveKeyframe{…}` | inverse; mergeable while dragging |
| `AddTransition` / `RemoveTransition` / `SetTransitionLength` | inverse |
| `AddMarker` / `RemoveMarker` / `SetMarker` | inverse |
| `AddAsset{path, folder}` / `RemoveAsset` / `RelinkAsset{assetId, newPath}` | inverse |

## Composite commands

Built from primitives inside a `Transaction`; each is one undo step.

- `RippleDelete{clipId}` = RemoveClip + MoveClip(-len) for every later clip on
  the same track (or on all unlocked tracks if "ripple all" is on).
- `RippleTrim{clipId, side, delta}` = ResizeClip + shift later clips.
- `InsertAt{track, asset, pos, in, out, mode}` with `mode = Overwrite |
  Insert`. Overwrite = split/trim/remove whatever the range covers, then
  InsertClip. Insert = shift everything at/after pos, then InsertClip.
- `DeleteSelection`, `MoveSelection{delta, trackDelta}`: applies to a set of
  clips; validates the *whole* move before applying any (a selection move
  that collides on one track is refused entirely).
- `LiftRange{trackIds, range}` / `ExtractRange` (three-point edit basics).

```cpp
class Transaction {          // used by composites and by the UI for "batch" gestures
public:
    explicit Transaction(Model&);      // emits BatchBegin
    bool run(std::unique_ptr<Command>);  // applies; on false → rollback() and return false
    void commit();                     // emits BatchEnd, keeps applied list for revert
    void rollback();                   // reverts applied commands in reverse order
};
```

## UndoStack

```cpp
class UndoStack {
public:
    bool execute(std::unique_ptr<Command>);  // apply; push; try merge with top; clear redo
    bool undo(); bool redo();
    bool canUndo() const; bool canRedo() const;
    std::string undoLabel() const; std::string redoLabel() const;
    void setCleanPoint(); bool isClean() const;   // for the dirty flag
    Signal<> changed;
    size_t limit = 500;                           // oldest dropped beyond this
};
```

Merging: `execute` asks `top->mergeWith(*cmd)`. `SetParam` merges when
`(target, effectId, name)` match and both were applied within 500 ms, so a
slider drag is one undo step. Merge windows close on any other command.

## Where commands come from

- **Timeline gestures**: `TimelineController` builds the command on release
  (doc 06). During the gesture it renders a preview overlay from a
  *candidate* command that it validates against the model each motion event
  via `Model::canMoveClip(...)`-style dry-run queries (pure functions on the
  model, no mutation), so illegal drops are shown as such before release.
- **Keyboard/menu actions**: `GAction`s in the app layer construct commands
  from the current selection and playhead.
- **Effect panel**: `SetParam` per widget change, merged.
- **Bin**: `AddAsset` after probing succeeds (asset appears immediately as
  `Pending`; a probe failure executes `RemoveAsset` and shows the error).

## Playback interaction

Executing a command while playing is allowed. `EngineSync` takes the tractor
lock; the consumer thread blocks for the duration of one track rebuild
(sub-millisecond for typical tracks, single-digit ms for thousands of clips).
Commands that change the sequence length also update the black backing track.
The playhead is clamped afterwards if it fell off the end.
