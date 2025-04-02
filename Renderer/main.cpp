#include <vector>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <SDL.h>

#include "tgaimage.h"
#include "model.h"
#include "geometry.h"
#include "our_gl.h"

#define M_PI 3.14159265358979323846

Vec3f light_dir(0, 1, 1);
Vec3f       eye(1, 0.5, 1.5);
Vec3f    center(0, 0, 0);
Vec3f        up(0, 1, 0);

float move_speed = 0.05f;
float rotate_speed = 2.0f * (M_PI / 180.0f);

// 帧率显示
Uint64 start_time = SDL_GetTicks();
int frame_count = 0;
float fps = 0.0f;

Model* model = nullptr;
TGAImage texture;

const int WIDTH = 800, HEIGHT = 800, DEPTH = 255;

// 高洛德着色 对每一个顶点
struct GouraudShader : public IShader
{
	Vec3f varying_intensity;
	mat<2, 3, float> varying_uv;
	// 顶点着色器
	virtual Vec4f vertex(int iface, int nthvert)
	{
		Vec4f gl_Vertex = embed<4>(model->vert(iface, nthvert));
        varying_uv.set_col(nthvert, model->uv(iface, nthvert));

		// 顶点坐标-》屏幕坐标
		mat<4, 4, float> uniform_M = Projection * ModelView;
		mat<4, 4, float> uniform_MIT = ModelView.invert_transpose();
		gl_Vertex = Viewport * uniform_M * gl_Vertex;
		// 计算光照强度
		Vec3f normal = proj<3>(embed<4>(model->normal(iface, nthvert))).normalize();
		varying_intensity[nthvert] = std::max(0.f, model->normal(iface, nthvert) * light_dir); // get diffuse lighting intensity
		return gl_Vertex;
	}
	// 片元着色器 根据光照强度和纹理 计算出当前像素的颜色
	virtual bool fragment(Vec3f bar, TGAColor& color) 
	{
		Vec2f uv = varying_uv * bar;
		TGAColor c = model->diffuse(uv);
		float intensity = varying_intensity * bar;
		color = c * intensity;
		return false;
	}
};

//将一定阈值内的光照强度给替换为一种
struct ToonShader : public IShader {
	mat<3, 3, float> varying_tri;
	Vec3f          varying_ity;

	virtual ~ToonShader() {}

	virtual Vec4f vertex(int iface, int nthvert) {
		Vec4f gl_Vertex = embed<4>(model->vert(iface, nthvert));
		gl_Vertex = Projection * ModelView * gl_Vertex;
		varying_tri.set_col(nthvert, proj<3>(gl_Vertex / gl_Vertex[3]));

		varying_ity[nthvert] = model->normal(iface, nthvert) * light_dir;

		gl_Vertex = Viewport * gl_Vertex;
		return gl_Vertex;
	}

	virtual bool fragment(Vec3f bar, TGAColor& color) {
		float intensity = varying_ity * bar;
		if (intensity > .85) intensity = 1;
		else if (intensity > .60) intensity = .80;
		else if (intensity > .45) intensity = .60;
		else if (intensity > .30) intensity = .45;
		else if (intensity > .15) intensity = .30;
		color = TGAColor(255, 155, 0) * intensity;
		return false;
	}
};

// Flat 着色 以面为单位进行着色
struct FlatShader : public IShader
{
	// 三角形顶点信息
	mat<3, 3, float> varying_tri;
	virtual ~FlatShader() {}

	virtual Vec4f vertex(int iface, int nthvert) {
		Vec4f gl_Vertex = embed<4>(model->vert(iface, nthvert));
		gl_Vertex = Projection * ModelView * gl_Vertex;
		varying_tri.set_col(nthvert, proj<3>(gl_Vertex / gl_Vertex[3]));
		gl_Vertex = Viewport * gl_Vertex;
		return gl_Vertex;
	}

	virtual bool fragment(Vec3f bar, TGAColor& color) {

		Vec3f n = cross(varying_tri.col(1) - varying_tri.col(0), varying_tri.col(2) - varying_tri.col(0)).normalize();
		float intensity = n * light_dir;
		color = TGAColor(255, 255, 255) * intensity;
		return false;
	}
};

// phong着色 以像素为单位进行着色
struct PhongShader : public IShader {
	mat<2, 3, float> varying_uv;  // same as above
	mat<4, 4, float> uniform_M = Projection * ModelView;
	mat<4, 4, float> uniform_MIT = ModelView.invert_transpose();
	virtual Vec4f vertex(int iface, int nthvert) {
		varying_uv.set_col(nthvert, model->uv(iface, nthvert));
		Vec4f gl_Vertex = embed<4>(model->vert(iface, nthvert)); // read the vertex from .obj file
		return Viewport * Projection * ModelView * gl_Vertex; // transform it to screen coordinates
	}
	virtual bool fragment(Vec3f bar, TGAColor& color) {
		Vec2f uv = varying_uv * bar;
		Vec3f n = proj<3>(uniform_MIT * embed<4>(model->normal(uv))).normalize();
		Vec3f l = proj<3>(uniform_M * embed<4>(light_dir)).normalize();
		Vec3f r = (n * (n * l * 2.f) - l).normalize();   // reflected light
		float spec = pow(std::max(r.z, 0.0f), model->specular(uv));
		float diff = std::max(0.f, n * l);
		TGAColor c = model->diffuse(uv);
		color = c;
		for (int i = 0; i < 3; i++) color[i] = std::min<float>(5 + c[i] * (diff + .6 * spec), 255);
		return false;
	}
};


int main(int argc, char** argv)
{
	model = new Model("./obj/african/african_head.obj");


	light_dir.normalize();
	TGAImage image(WIDTH, HEIGHT, TGAImage::RGB);
	TGAImage zbuffer(WIDTH, HEIGHT, TGAImage::GRAYSCALE);

	// 实例化着色器
	GouraudShader shader;

	// MVP矩阵
	lookat(eye, center, up);
	projection(-1.f / (eye - center).norm());
	viewport(WIDTH / 8, HEIGHT / 8, WIDTH * 3 / 4, HEIGHT * 3 / 4);

	// 循环每一个面
	for (int i = 0; i < model->nfaces(); i++)
	{
		Vec4f screen_coords[3];
		for (int j = 0; j < 3; j++) {
			//通过顶点着色器读取模型顶点
			//变换顶点坐标到屏幕坐标（视角矩阵*投影矩阵*变换矩阵*v） ***其实并不是真正的屏幕坐标，因为没有除以最后一个分量
			//计算光照强度
			screen_coords[j] = shader.vertex(i, j);
		}
		//绘制三角形，triangle内部通过片元着色器对三角形着色
		triangle(screen_coords, shader, image, zbuffer);
	}

	image.flip_vertically();
	zbuffer.flip_vertically();
	image.write_tga_file("./output/output.png");
	//zbuffer.write_tga_file("./output/zbuffer.tga");
	return 0;
}


