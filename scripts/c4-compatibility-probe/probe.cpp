// Independent offscreen OpenGL experiment. No Minecraft or Nova source is linked.
// Tests the texture allocation/sampling hypothesis identified by client inspection.
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context; context.setFormat(format);
    if (!context.create()) return 2;
    QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
    if (!context.makeCurrent(&surface)) return 3;
    QOpenGLFunctions_3_3_Core gl; if (!gl.initializeOpenGLFunctions()) return 4;
    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex,
        "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}") ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment,
        "#version 330 core\nuniform sampler2D src;out vec4 pixel;void main(){pixel=textureLod(src,vec2(0.5),0.0);}") ||
        !program.link()) return 5;
    GLuint framebuffer=0, target=0, vao=0;
    gl.glGenFramebuffers(1,&framebuffer); gl.glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    gl.glGenTextures(1,&target); gl.glBindTexture(GL_TEXTURE_2D,target);
    gl.glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 6;
    gl.glGenVertexArrays(1,&vao); gl.glBindVertexArray(vao); gl.glViewport(0,0,4,4);
    program.bind(); program.setUniformValue("src",0);
    QJsonArray rows;
    for (const auto size : {std::array{16,8},std::array{8,16},std::array{32,8},std::array{16,16}}) {
        for (int requested : {0,1,2,3,4}) {
            int effective=std::min(requested,3);
            if (size[0]==16 && size[1]==16) effective=requested;
            for (bool extra : {true,false}) {
                // At effective mip M, the client path requests M+2 allocations;
                // the control requests only the M+1 uploaded levels.
                const int count=effective+1+int(extra);
                GLuint source=0; gl.glGenTextures(1,&source); gl.glBindTexture(GL_TEXTURE_2D,source);
                gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,count-1);
                gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_LINEAR);
                gl.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                QJsonArray levels;
                for(int level=0;level<count;++level) {
                    int w=size[0]>>level,h=size[1]>>level;
                    levels.append(QJsonArray{w,h});
                    gl.glTexImage2D(GL_TEXTURE_2D,level,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
                    if(level<=effective) {
                        std::vector<unsigned char> rgba(size_t(w*h*4),255);
                        for(size_t p=0;p<rgba.size();p+=4){rgba[p+1]=40;rgba[p+2]=30;}
                        gl.glTexSubImage2D(GL_TEXTURE_2D,level,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
                    }
                }
                const auto allocationError=gl.glGetError();
                gl.glClearColor(0,1,0,1); gl.glClear(GL_COLOR_BUFFER_BIT);
                gl.glDrawArrays(GL_TRIANGLES,0,3);
                std::array<unsigned char,4> pixel{};
                gl.glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
                const auto renderError=gl.glGetError();
                rows.append(QJsonObject{{"frame",QJsonArray{size[0],size[1]}},
                    {"requested",requested},{"effective",effective},{"extra_allocation",extra},
                    {"levels",levels},{"allocation_gl_error",int(allocationError)},
                    {"render_gl_error",int(renderError)},
                    {"sampled_rgba",QJsonArray{pixel[0],pixel[1],pixel[2],pixel[3]}}});
                gl.glDeleteTextures(1,&source);
            }
        }
    }
    auto info=[&](GLenum name){return QString::fromLatin1(reinterpret_cast<const char*>(gl.glGetString(name)));};
    QJsonObject result{{"kind","offscreen backend mechanism probe; not Minecraft runtime verification"},
        {"vendor",info(GL_VENDOR)},{"renderer",info(GL_RENDERER)},{"version",info(GL_VERSION)},
        {"rows",rows}};
    auto bytes=QJsonDocument(result).toJson(); std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);
    return 0;
}
