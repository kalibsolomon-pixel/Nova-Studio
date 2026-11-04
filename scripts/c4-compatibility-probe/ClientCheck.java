// Independent harness calling the installed official 1.21.11 client, read-only.
// No game implementation is copied here; obfuscated symbols are version specific.
package c4;
import com.google.gson.*;
import com.mojang.serialization.Codec;
import com.mojang.serialization.JsonOps;
import java.lang.reflect.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

public class ClientCheck {
    static Class<?> nativeImage;
    static int width(Object image) throws Exception {return (int)nativeImage.getMethod("a").invoke(image);}
    static int height(Object image) throws Exception {return (int)nativeImage.getMethod("b").invoke(image);}
    static int pixel(Object image,int x,int y) throws Exception {return (int)nativeImage.getMethod("a",int.class,int.class).invoke(image,x,y);}
    static String rgbaHash(Object image) throws Exception {
        var digest=MessageDigest.getInstance("SHA-256");
        for(int y=0;y<height(image);y++) for(int x=0;x<width(image);x++) {
            int p=pixel(image,x,y);
            digest.update(new byte[]{(byte)(p>>16),(byte)(p>>8),(byte)p,(byte)(p>>24)});
        }
        return HexFormat.of().formatHex(digest.digest());
    }
    static JsonObject info(Object image) throws Exception {
        var value=new JsonObject(); value.addProperty("width",width(image));value.addProperty("height",height(image));
        value.addProperty("rgba_sha256",rgbaHash(image));
        var counts=new TreeMap<Integer,Integer>();
        for(int y=0;y<height(image);y++)for(int x=0;x<width(image);x++)counts.merge(pixel(image,x,y)>>>24,1,Integer::sum);
        value.add("alpha_counts",new Gson().toJsonTree(counts));return value;
    }
    public static void main(String[] args) throws Exception {
        var output=new JsonArray();
        nativeImage=Class.forName("fyh");
        var animationType=Class.forName("imz");var textureType=Class.forName("inj");
        var animationCodec=(Codec<?>)animationType.getField("a").get(null);
        var textureCodec=(Codec<?>)textureType.getField("d").get(null);
        try(var directories=Files.list(Path.of(args[0]))) {
            for(var directory:directories.sorted().toList()) {
                var json=JsonParser.parseString(Files.readString(directory.resolve("texture.png.mcmeta"))).getAsJsonObject();
                var animation=animationCodec.parse(JsonOps.INSTANCE,json.get("animation")).getOrThrow();
                var texture=textureCodec.parse(JsonOps.INSTANCE,json.get("texture")).getOrThrow();
                Object image=nativeImage.getMethod("a",byte[].class).invoke(null,(Object)Files.readAllBytes(directory.resolve("texture.png")));
                try {
                    var row=new JsonObject(); row.addProperty("variant",directory.getFileName().toString());
                    row.addProperty("animation_codec_accepted",true);row.addProperty("texture_codec_accepted",true);
                    var size=animationType.getMethod("a",int.class,int.class).invoke(animation,width(image),height(image));
                    row.add("frame",new Gson().toJsonTree(new int[]{(int)size.getClass().getMethod("a").invoke(size),(int)size.getClass().getMethod("b").invoke(size)}));
                    row.add("decoded",info(image));
                    Object strategy=textureType.getMethod("c").invoke(texture);float bias=(float)textureType.getMethod("d").invoke(texture);
                    row.addProperty("strategy",(String)strategy.getClass().getMethod("c").invoke(strategy));row.addProperty("bias",bias);
                    var identifierType=Class.forName("amo");var identifier=identifierType.getMethod("a",String.class,String.class).invoke(null,"nova","block/"+directory.getFileName());
                    Object images=Array.newInstance(nativeImage,1);Array.set(images,0,image);
                    var levels=(Object[])Class.forName("ild").getMethod("a",identifierType,images.getClass(),int.class,strategy.getClass(),float.class).invoke(null,identifier,images,3,strategy,bias);
                    var mips=new JsonArray();for(var level:levels)mips.add(info(level));row.add("cpu_mips",mips);
                    for(int i=1;i<levels.length;i++)((AutoCloseable)levels[i]).close();
                    output.add(row);
                } finally {((AutoCloseable)image).close();}
            }
        }
        Files.writeString(Path.of(args[1]),new GsonBuilder().setPrettyPrinting().create().toJson(output)+"\n");
    }
}
