// Calls the actual official-client allocation entry point in a separate JVM.
// Recording sinks replace hardware calls only in this diagnostic process.
package c4;
import com.google.gson.*;
import com.mojang.serialization.Codec;
import com.mojang.serialization.JsonOps;
import com.mojang.blaze3d.systems.*;
import com.mojang.blaze3d.textures.*;
import com.mojang.blaze3d.buffers.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;

public class AllocationTrace {
    static class Texture extends GpuTexture {
        boolean closed;
        Texture(int usage,TextureFormat format,int w,int h,int depth,int mips){super(usage,"C4 recording texture",format,w,h,depth,mips);}
        public void close(){closed=true;} public boolean isClosed(){return closed;}
    }
    static class View extends GpuTextureView {
        boolean closed;View(GpuTexture t,int base,int count){super(t,base,count);}
        public void close(){closed=true;} public boolean isClosed(){return closed;}
    }
    static class Buffer extends GpuBuffer {
        boolean closed;Buffer(long size){super(0,size);}
        public void close(){closed=true;} public boolean isClosed(){return closed;}
    }
    public static void main(String[] args)throws Exception {
        var all=new JsonArray();var allocations=new JsonArray();var uploads=new JsonArray();
        var encoder=(CommandEncoder)Proxy.newProxyInstance(AllocationTrace.class.getClassLoader(),new Class[]{CommandEncoder.class},(p,m,a)->{
            if(m.getName().equals("writeToTexture")){
                var row=new JsonObject();row.addProperty("mip_level",(int)a[2]);
                row.addProperty("width",(int)a[6]);row.addProperty("height",(int)a[7]);uploads.add(row);
            }return null;
        });
        var device=(GpuDevice)Proxy.newProxyInstance(AllocationTrace.class.getClassLoader(),new Class[]{GpuDevice.class},(p,m,a)->{
            if(m.getName().equals("createTexture")){
                int w=(int)a[3],h=(int)a[4],count=(int)a[6];
                var row=new JsonObject();row.addProperty("width",w);row.addProperty("height",h);row.addProperty("requested_levels",count);
                allocations.add(row);return new Texture((int)a[1],(TextureFormat)a[2],w,h,(int)a[5],count);
            }
            if(m.getName().equals("createTextureView"))return new View((GpuTexture)a[0],a.length==1?0:(int)a[1],a.length==1?((GpuTexture)a[0]).getMipLevels():(int)a[2]);
            if(m.getName().equals("createCommandEncoder"))return encoder;
            throw new UnsupportedOperationException(m.toString());
        });
        var field=RenderSystem.class.getDeclaredField("DEVICE");field.setAccessible(true);field.set(null,device);
        var nativeType=Class.forName("fyh");var animType=Class.forName("imz");var textureType=Class.forName("inj");
        var frameType=Class.forName("ina");var idType=Class.forName("amo");var contentsType=Class.forName("ilk");
        var animCodec=(Codec<?>)animType.getField("a").get(null);var textureCodec=(Codec<?>)textureType.getField("d").get(null);
        for(String variant:List.of("nova_original","reference","square","tall"))for(int mip:new int[]{2,3,4}) {
            int effective=variant.equals("square")?mip:Math.min(mip,3);
            var directory=Path.of(args[0],variant);var metadata=JsonParser.parseString(Files.readString(directory.resolve("texture.png.mcmeta"))).getAsJsonObject();
            Object image=nativeType.getMethod("a",byte[].class).invoke(null,(Object)Files.readAllBytes(directory.resolve("texture.png")));
            Object anim=animCodec.parse(JsonOps.INSTANCE,metadata.get("animation")).getOrThrow();
            Object texture=textureCodec.parse(JsonOps.INSTANCE,metadata.get("texture")).getOrThrow();
            int width=(int)nativeType.getMethod("a").invoke(image),height=(int)nativeType.getMethod("b").invoke(image);
            Object frame=animType.getMethod("a",int.class,int.class).invoke(anim,width,height);
            Object id=idType.getMethod("a",String.class,String.class).invoke(null,"nova","block/trace");
            Object contents=contentsType.getConstructor(idType,frameType,nativeType,Optional.class,List.class,Optional.class).newInstance(id,frame,image,Optional.of(anim),List.of(),Optional.of(texture));
            contentsType.getMethod("a",int.class).invoke(contents,effective);
            int firstAllocation=allocations.size(),firstUpload=uploads.size();
            var state=(AutoCloseable)contentsType.getMethod("a",GpuBufferSlice.class,int.class).invoke(contents,new GpuBufferSlice(new Buffer((effective+1)*256L),0,(effective+1)*256L),256);
            var row=new JsonObject();row.addProperty("variant",variant);row.addProperty("requested",mip);row.addProperty("effective",effective);
            var a=new JsonArray();for(int i=firstAllocation;i<allocations.size();i++)a.add(allocations.get(i));
            var u=new JsonArray();for(int i=firstUpload;i<uploads.size();i++)u.add(uploads.get(i));
            row.add("actual_client_allocations",a);row.add("actual_client_uploads",u);all.add(row);
            state.close();((AutoCloseable)contents).close();
        }
        Files.writeString(Path.of(args[1]),new GsonBuilder().setPrettyPrinting().create().toJson(all)+"\n");
    }
}
