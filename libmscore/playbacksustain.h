//=============================================================================
//  MuseScore
//  Playback-only sustain overrides used by the key editor
//=============================================================================

#ifndef __PLAYBACKSUSTAIN_H__
#define __PLAYBACKSUSTAIN_H__

#include "synthesizerstate.h"

#include <QStringList>
#include <QVector>

#include <limits>

namespace Ms {

struct PlaybackSustainSpan {
      int startTick { 0 };
      int endTick { 1 };
      // A non-negative source range identifies the notation pedal this
      // playback-only span replaces. -1 means a raw CC64 span created in the
      // key editor; -2 is the legacy staff-wide snapshot format.
      int sourceStartTick { -1 };
      int sourceEndTick { -1 };

      bool notationLinked() const { return sourceStartTick >= 0 && sourceEndTick > sourceStartTick; }
      bool legacy() const         { return sourceStartTick == -2; }
      bool suppressed() const     { return startTick < 0 || endTick <= startTick; }
      };

static const char* PLAYBACK_SUSTAIN_GROUP = "KeyEditorSustain";

inline bool playbackSustainSpans(const SynthesizerState& state, int staffIdx,
                                 QVector<PlaybackSustainSpan>* spans)
      {
      if (spans)
            spans->clear();
      const SynthesizerGroup group = state.group(QLatin1String(PLAYBACK_SUSTAIN_GROUP));
      for (const IdValue& value : group) {
            if (value.id != staffIdx)
                  continue;
            if (spans) {
                  const QStringList encodedSpans = value.data.split(QLatin1Char(';'), QString::SkipEmptyParts);
                  for (const QString& encoded : encodedSpans) {
                        const QStringList bounds = encoded.split(QLatin1Char(','));
                        if (bounds.size() != 2 && bounds.size() != 4)
                              continue;
                        bool startOk = false;
                        bool endOk = false;
                        const int start = bounds[0].toInt(&startOk);
                        const int end = bounds[1].toInt(&endOk);
                        if (!startOk || !endOk)
                              continue;
                        if (bounds.size() == 2) {
                              if (end > start)
                                    spans->append({ start, end, -2, -2 });
                              continue;
                              }
                        bool sourceStartOk = false;
                        bool sourceEndOk = false;
                        const int sourceStart = bounds[2].toInt(&sourceStartOk);
                        const int sourceEnd = bounds[3].toInt(&sourceEndOk);
                        const bool validPlayback = end > start || (start == -1 && end == -1);
                        const bool validSource = (sourceStart == -1 && sourceEnd == -1)
                                              || (sourceStart >= 0 && sourceEnd > sourceStart);
                        if (sourceStartOk && sourceEndOk && validPlayback && validSource)
                              spans->append({ start, end, sourceStart, sourceEnd });
                        }
                  }
            return true;
            }
      return false;
      }

// Convert old staff-wide snapshots to individual notation-linked overrides
// and discard links whose notation source no longer exists. This keeps score
// edits authoritative without changing the notation when the key editor is
// used for playback-only adjustment.
inline QVector<PlaybackSustainSpan> resolvePlaybackSustainSpans(
   const QVector<PlaybackSustainSpan>& stored,
   const QVector<PlaybackSustainSpan>& notation)
      {
      QVector<PlaybackSustainSpan> resolved;
      QVector<bool> sourceUsed(notation.size(), false);
      QVector<PlaybackSustainSpan> legacy;

      for (const PlaybackSustainSpan& span : stored) {
            if (span.legacy()) {
                  legacy.append(span);
                  continue;
                  }
            if (!span.notationLinked()) {
                  resolved.append(span);
                  continue;
                  }
            for (int i = 0; i < notation.size(); ++i) {
                  if (notation[i].startTick == span.sourceStartTick
                      && notation[i].endTick == span.sourceEndTick) {
                        resolved.append(span);
                        sourceUsed[i] = true;
                        break;
                        }
                  }
            }

      // Legacy files stored a sorted copy of every notation pedal. Pair each
      // surviving source with the nearest stored span. Unmatched legacy spans
      // are stale snapshots and must not outlive deleted notation.
      QVector<bool> legacyUsed(legacy.size(), false);
      for (int source = 0; source < notation.size(); ++source) {
            if (sourceUsed[source])
                  continue;
            int best = -1;
            qint64 bestDistance = std::numeric_limits<qint64>::max();
            for (int candidate = 0; candidate < legacy.size(); ++candidate) {
                  if (legacyUsed[candidate])
                        continue;
                  const qint64 distance = qAbs(qint64(legacy[candidate].startTick)
                                                - notation[source].startTick)
                                        + qAbs(qint64(legacy[candidate].endTick)
                                                - notation[source].endTick);
                  if (distance < bestDistance) {
                        best = candidate;
                        bestDistance = distance;
                        }
                  }
            if (best >= 0) {
                  PlaybackSustainSpan migrated = legacy[best];
                  migrated.sourceStartTick = notation[source].startTick;
                  migrated.sourceEndTick = notation[source].endTick;
                  resolved.append(migrated);
                  legacyUsed[best] = true;
                  sourceUsed[source] = true;
                  }
            }
      return resolved;
      }

inline void setPlaybackSustainSpans(SynthesizerState& state, int staffIdx,
                                    const QVector<PlaybackSustainSpan>& spans)
      {
      SynthesizerGroup group = state.group(QLatin1String(PLAYBACK_SUSTAIN_GROUP));
      group.setName(QLatin1String(PLAYBACK_SUSTAIN_GROUP));
      for (auto it = group.begin(); it != group.end();) {
            if (it->id == staffIdx)
                  it = group.erase(it);
            else
                  ++it;
            }

      QStringList encoded;
      for (const PlaybackSustainSpan& span : spans) {
            const bool validPlayback = span.endTick > span.startTick
                                    || (span.startTick == -1 && span.endTick == -1);
            const bool validSource = (span.sourceStartTick == -1 && span.sourceEndTick == -1)
                                  || (span.sourceStartTick >= 0
                                      && span.sourceEndTick > span.sourceStartTick);
            if (validPlayback && validSource)
                  encoded.append(QString::number(span.startTick) + QLatin1Char(',')
                               + QString::number(span.endTick) + QLatin1Char(',')
                               + QString::number(span.sourceStartTick) + QLatin1Char(',')
                               + QString::number(span.sourceEndTick));
            }
      if (!encoded.isEmpty())
            group.push_back(IdValue(staffIdx, encoded.join(QLatin1Char(';'))));

      for (auto it = state.begin(); it != state.end();) {
            if (it->name() == QLatin1String(PLAYBACK_SUSTAIN_GROUP))
                  it = state.erase(it);
            else
                  ++it;
            }
      if (!group.empty())
            state.push_back(group);
      state.setIsDefault(false);
      }

} // namespace Ms

#endif
