//=============================================================================
//  MuseScore
//  Playback-only sustain overrides used by the key editor
//=============================================================================

#ifndef __PLAYBACKSUSTAIN_H__
#define __PLAYBACKSUSTAIN_H__

#include "synthesizerstate.h"

#include <QStringList>
#include <QVector>

namespace Ms {

struct PlaybackSustainSpan {
      int startTick { 0 };
      int endTick { 1 };
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
                        if (bounds.size() != 2)
                              continue;
                        bool startOk = false;
                        bool endOk = false;
                        const int start = bounds[0].toInt(&startOk);
                        const int end = bounds[1].toInt(&endOk);
                        if (startOk && endOk && end > start)
                              spans->append({ start, end });
                        }
                  }
            return true;
            }
      return false;
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
            if (span.endTick > span.startTick)
                  encoded.append(QString::number(span.startTick) + QLatin1Char(',')
                               + QString::number(span.endTick));
            }
      group.push_back(IdValue(staffIdx, encoded.join(QLatin1Char(';'))));

      for (auto it = state.begin(); it != state.end();) {
            if (it->name() == QLatin1String(PLAYBACK_SUSTAIN_GROUP))
                  it = state.erase(it);
            else
                  ++it;
            }
      state.push_back(group);
      state.setIsDefault(false);
      }

} // namespace Ms

#endif
