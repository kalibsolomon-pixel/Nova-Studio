// Artifact integration verification against the existing Release core library.
// No GUI, native documents, original exports or game instance are modified.
#include "minecraft_animation.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
using namespace nova;
QByteArray read(const QString &path) { QFile f(path); if(!f.open(QIODevice::ReadOnly))return {}; return f.readAll(); }
bool write(const QString &path,const QByteArray &bytes) { QFile f(path); return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size(); }
QString hash(const QImage &image) { auto rgba=image.convertToFormat(QImage::Format_RGBA8888);return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(rgba.constBits()),rgba.sizeInBytes()),QCryptographicHash::Sha256).toHex(); }
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);if(argc!=4)return 2;
    const QString manifest=QString::fromLocal8Bit(argv[1]),input=QString::fromLocal8Bit(argv[2]),output=QString::fromLocal8Bit(argv[3]);
    if(QFileInfo::exists(output)||!QDir().mkpath(output))return 3;
    QJsonArray results;bool passed=true;
    for(const auto &value:QJsonDocument::fromJson(read(manifest)).array()) {
        const auto row=value.toObject();const auto name=row["name"].toString();const auto path=input+"/"+name+"/texture.png";
        QImage sheet(path);sheet=sheet.convertToFormat(QImage::Format_RGBA8888);
        QString error;auto doc=openMinecraftAnimation(path,"1.21.11",&error);
        bool ok=bool(doc);QJsonArray frames;
        AnimationExport plan;
        if(doc) {
            const auto state=doc->animationState();int i=0;const int columns=row["columns"].toInt();const auto dim=row["frame"].toArray();
            for(const auto &frame:state.frames) {
                const auto expected=sheet.copy((i%columns)*dim[0].toInt(),(i/columns)*dim[1].toInt(),dim[0].toInt(),dim[1].toInt());
                const auto actual=doc->frameComposite(frame.id);const bool equal=actual==expected;ok&=equal;
                frames.append(QJsonObject{{"index",i},{"expected_rgba_sha256",hash(expected)},{"actual_rgba_sha256",hash(actual)},{"equal",equal}});++i;
            }
            plan=planAnimationExport(*doc,columns,"1.21.11",&error);ok&=plan.ok()&&plan.sheet==sheet;
            ok&=plan.metadata==row["metadata"].toObject();
            const auto dest=output+"/"+name;ok&=QDir().mkpath(dest);
            ok&=write(dest+"/texture.png",plan.png)&&write(dest+"/texture.png.mcmeta",plan.mcmeta);
            auto again=openMinecraftAnimation(dest+"/texture.png","1.21.11",&error);ok&=bool(again);
            if(again) {
                const auto restored=again->animationState();ok&=restored.defaultTicks==state.defaultTicks&&restored.interpolate==state.interpolate;
                ok&=restored.frames.size()==state.frames.size()&&restored.entries.size()==state.entries.size();
                if(restored.frames.size()==state.frames.size())for(size_t n=0;n<state.frames.size();++n)
                    ok&=again->frameComposite(restored.frames[n].id)==doc->frameComposite(state.frames[n].id);
                if(restored.entries.size()==state.entries.size())for(size_t n=0;n<state.entries.size();++n)
                    ok&=restored.entries[n].frame==restored.frames[n].id&&restored.entries[n].ticks==state.entries[n].ticks;
            }
        }
        results.append(QJsonObject{{"variant",name},{"passed",ok},{"error",error},{"frames",frames},
            {"sheet_exact",plan.sheet==sheet},{"metadata_exact",plan.metadata==row["metadata"].toObject()}});passed&=ok;
    }
    write(output+"/results.json",QJsonDocument(QJsonObject{{"passed",passed},{"variants",results}}).toJson());
    std::printf("%d artifact import/export/reimport cases: %s\n",int(results.size()),passed?"passed":"FAILED");
    return passed?0:1;
}
