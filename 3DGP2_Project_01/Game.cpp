#include "stdafx.h"
#include "Geometry.h"
#include <DirectXCollision.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

namespace {
enum class Scene { Intro, Menu, Tutorial, Level1, Level2, Level3 };
enum class Result { Playing, Won, Lost };
constexpr XMFLOAT4 Mint{.48f, 1.f, .84f, 1.f};
constexpr XMFLOAT4 Amber{1.f, .73f, .35f, 1.f};
constexpr XMFLOAT4 Ice{.64f, .84f, 1.f, 1.f};
constexpr XMFLOAT4 Red{1.f, .43f, .34f, 1.f};

XMFLOAT3 Store(FXMVECTOR vector) {
    XMFLOAT3 value;
    XMStoreFloat3(&value, vector);
    return value;
}
XMVECTOR Vec(const XMFLOAT3& value) { return XMLoadFloat3(&value); }
XMMATRIX At(const XMFLOAT3& p) { return XMMatrixTranslation(p.x, p.y, p.z); }
bool Held(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
float GroundDistance(const XMFLOAT3& a, const XMFLOAT3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}
float YawTo(const XMFLOAT3& a, const XMFLOAT3& b) {
    return std::atan2(b.x - a.x, b.z - a.z);
}

struct Unit {
    XMFLOAT3 position{};
    float turret = 0;
    float fireTimer = 0;
    int hp = 100;
};
struct Shot {
    XMFLOAT3 position, velocity;
    float remaining = 3.f;
    bool friendly;
};
struct Fragment {
    XMFLOAT3 position, velocity;
    float age, life, size;
};
struct Obstacle {
    XMFLOAT3 position;
    float size, yaw;
};
}

class Game {
public:
    void Init(HWND window);
    void Update(float dt);
    void Render();
    void Key(WPARAM key);
    void Click(int x, int y, bool right);
    void Drag(int dx, int dy);

private:
    void Enter(Scene next);
    void UpdateCamera();
    void Fire(const XMFLOAT3& origin, FXMVECTOR direction, bool friendly);
    void UpdateShots(float dt);
    void Burst(const XMFLOAT3& position, float size, bool major);
    void DrawTank(const Unit& unit, float hullYaw, bool friendly);
    void DrawHelicopter();
    void DrawText3D(const TextMesh& text, const XMMATRIX& world, const XMFLOAT4& color);
    void DrawFragments();
    XMMATRIX IntroName() const;
    XMMATRIX MenuItem(size_t index) const;
    XMFLOAT3 EnemyCenter(size_t index) const;
    XMFLOAT3 BalloonCenter(size_t index) const;
    XMFLOAT3 PlayerCenter() const;
    bool IsHelicopter() const { return scene == Scene::Level1 || scene == Scene::Level3; }
    bool IsLevel() const { return scene >= Scene::Level1; }
    int Remaining() const;

    Renderer renderer;
    Mesh box, sphere, terrain;
    std::array<Mesh, 3> tank, helicopter;
    TextMesh title{}, name{}, introHint{}, menuTitle{}, winText{}, lostText{}, goalText{};
    std::array<TextMesh, 6> menu;
    std::array<TextMesh, 6> tutorial;
    Scene scene = Scene::Intro;
    Result result = Result::Playing;
    Unit player;
    std::vector<Unit> enemies, balloons;
    std::vector<Shot> shots;
    std::vector<Fragment> fragments;
    std::vector<Obstacle> obstacles;
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX viewProjection = XMMatrixIdentity();
    float yaw = 0, pitch = 0, time = 0, introExplosion = -1;
    int target = -1;
    bool automatic = false, shield = false, firstPerson = false, firePressed = false;
    std::mt19937 random{42};
};

void Game::Init(HWND window) {
    renderer.Init(window);
    box = MakeBox(renderer);
    sphere = MakeSphere(renderer);
    terrain = MakeTerrain(renderer);
    tank = {
        LoadObj(renderer, "Assets/Tank/T04_Hull_LOD0.obj"),
        LoadObj(renderer, "Assets/Tank/T04_Turret_LOD0.obj"),
        LoadObj(renderer, "Assets/Tank/T04_Gun_LOD0.obj")
    };
    helicopter = {
        LoadObj(renderer, "Assets/Helicopter/H04_Body_LOD0.obj"),
        LoadObj(renderer, "Assets/Helicopter/H04_MainRotor_LOD0.obj"),
        LoadObj(renderer, "Assets/Helicopter/H04_TailRotor_LOD0.obj")
    };
    title = Make3DText(renderer, L"3D 게임프로그래밍 1");
    name = Make3DText(renderer, L"김준해");
    introHint = Make3DText(renderer, L"CLICK THE NAME");
    menuTitle = Make3DText(renderer, L"MENU");
    winText = Make3DText(renderer, L"You Win!");
    lostText = Make3DText(renderer, L"Game Over");
    goalText = Make3DText(renderer, L"GOAL");

    const wchar_t* labels[] = {L"Tutorial", L"Level-1", L"Level-2", L"Level-3", L"Start", L"End"};
    for (size_t index = 0; index < menu.size(); ++index) menu[index] = Make3DText(renderer, labels[index]);
    const wchar_t* instructions[] = {
        L"ARROWS   MOVE",
        L"LEFT DRAG   TURN / AIM",
        L"SPACE   FIRE     E / Q   UP / DOWN",
        L"TANK   RMB   SELECT     A   AUTO     S   SHIELD",
        L"N   NEXT LEVEL     V   CAMERA     W   WIN",
        L"ESC   BACK"
    };
    for (size_t index = 0; index < tutorial.size(); ++index)
        tutorial[index] = Make3DText(renderer, instructions[index]);

    Enter(Scene::Intro);
}

void Game::Enter(Scene next) {
    scene = next;
    result = Result::Playing;
    time = 0;
    introExplosion = -1;
    shots.clear();
    fragments.clear();
    enemies.clear();
    balloons.clear();
    obstacles.clear();
    player = {};
    yaw = pitch = 0;
    target = -1;
    automatic = shield = firstPerson = firePressed = false;

    if (IsLevel()) {
        player.position = {0, IsHelicopter() ? 10.f : GroundHeight(0, -9), -9};
        if (scene == Scene::Level1) {
            for (int index = 0; index < 9; ++index) {
                Unit balloon;
                balloon.position = {float(index % 3 - 1) * 9, 11.f + float(index / 3) * 2, 13.f + float(index / 3) * 13};
                balloon.hp = 1;
                balloons.push_back(balloon);
            }
        } else {
            for (int index = 0; index < 12; ++index) {
                Unit enemy;
                const float x = float(index % 5 - 2) * 9;
                const float z = 15.f + float(index / 5) * 13;
                enemy.position = {x, GroundHeight(x, z), z};
                enemy.hp = 3;
                enemy.fireTimer = 1.5f + float(index % 6) * .6f;
                enemies.push_back(enemy);
            }
        }

        // Keep the route and the tank spawn positions clear.
        std::mt19937 placement(4);
        std::uniform_real_distribution<float> position(-68.f, 68.f), size(1.1f, 2.8f);
        const size_t count = scene == Scene::Level2 ? 55 : 26;
        while (obstacles.size() < count) {
            const float x = position(placement), z = position(placement);
            if (std::abs(x) < 24 && z > -20 && z < 53) continue;
            const float scale = size(placement);
            obstacles.push_back({{x, GroundHeight(x, z) + scale * .5f, z}, scale, x * .12f});
        }
    }
    UpdateCamera();
}

XMFLOAT3 Game::PlayerCenter() const {
    auto center = player.position;
    center.y += IsHelicopter() ? 1.25f : 1.3f;
    return center;
}

XMFLOAT3 Game::EnemyCenter(size_t index) const {
    auto center = enemies[index].position;
    center.y += 1.3f;
    return center;
}

XMFLOAT3 Game::BalloonCenter(size_t index) const {
    auto center = balloons[index].position;
    center.y += std::sin(time * 2.f + float(index)) * .45f;
    return center;
}

void Game::UpdateCamera() {
    XMVECTOR from = XMVectorSet(0, 0, -16, 0);
    XMVECTOR to = XMVectorZero();
    if (IsLevel()) {
        const XMVECTOR forward = XMVectorSet(std::sin(yaw), 0, std::cos(yaw), 0);
        const XMVECTOR position = Vec(player.position);
        if (firstPerson) {
            from = position + forward * 2.35f + XMVectorSet(0, 2.35f, 0, 0);
            to = from + forward * 10.f + XMVectorSet(0, std::tan(pitch) * 10.f, 0, 0);
        } else {
            from = position - forward * (IsHelicopter() ? 14.f : 12.f) + XMVectorSet(0, IsHelicopter() ? 5.2f : 6.4f, 0, 0);
            XMFLOAT3 p = Store(from);
            p.y = std::max(p.y, GroundHeight(p.x, p.z) + 2.f);
            from = Vec(p);
            to = position + forward * 11.f + XMVectorSet(0, 1.9f + pitch * 13.f, 0, 0);
        }
    }
    view = XMMatrixLookAtLH(from, to, XMVectorSet(0, 1, 0, 0));
    viewProjection = view *
        XMMatrixPerspectiveFovLH(XMConvertToRadians(57), float(Width) / Height, .15f, 380.f);
}

void Game::Key(WPARAM key) {
    if (key == VK_SPACE && IsLevel()) firePressed = true;
    if (key == VK_ESCAPE) {
        if (scene == Scene::Intro) PostQuitMessage(0);
        else if (scene == Scene::Menu) Enter(Scene::Intro);
        else Enter(Scene::Menu);
        return;
    }
    if (scene == Scene::Level1 && key == 'N') Enter(Scene::Level2);
    if (scene == Scene::Level2 && !Held(VK_SHIFT)) {
        if (key == 'A') automatic = !automatic;
        if (key == 'S') shield = !shield;
        if (key == 'W' && result == Result::Playing) result = Result::Won;
    }
    if (scene == Scene::Level3 && key == 'V') firstPerson = !firstPerson;
    if (IsLevel() && result != Result::Playing && key == 'R') Enter(scene);
}

XMMATRIX Game::IntroName() const {
    return XMMatrixRotationY(.65f * std::sin(time * 2.4f)) * XMMatrixTranslation(0, -.45f, 0);
}

XMMATRIX Game::MenuItem(size_t index) const {
    return XMMatrixScaling(.72f, .72f, .72f) * XMMatrixTranslation(0, 3.05f - 1.46f * float(index), 0);
}

void Game::Click(int x, int y, bool right) {
    const XMMATRIX inverse = XMMatrixInverse(nullptr, viewProjection);
    const auto nearPoint = XMVector3TransformCoord(XMVectorSet(2.f * x / Width - 1.f, 1.f - 2.f * y / Height, 0, 1), inverse);
    const auto farPoint = XMVector3TransformCoord(XMVectorSet(2.f * x / Width - 1.f, 1.f - 2.f * y / Height, 1, 1), inverse);
    const auto direction = XMVector3Normalize(farPoint - nearPoint);
    auto textHit = [&](const TextMesh& text, const XMMATRIX& world) {
        const auto local = XMMatrixInverse(nullptr, world);
        const BoundingBox bounds({0,0,0}, {text.width / 2 + .25f, text.height / 2 + .18f, .2f});
        float distance = 0;
        return bounds.Intersects(XMVector3TransformCoord(nearPoint, local),
            XMVector3Normalize(XMVector3TransformNormal(direction, local)), distance);
    };

    if (!right && scene == Scene::Intro && introExplosion < 0 && textHit(name, IntroName())) {
        introExplosion = time;
        Burst({0, -.45f, 0}, 6.f, true);
    }
    if (!right && scene == Scene::Menu) {
        const Scene destinations[] = {Scene::Tutorial, Scene::Level1, Scene::Level2, Scene::Level3, Scene::Level1};
        for (size_t index = 0; index < menu.size(); ++index) {
            if (!textHit(menu[index], MenuItem(index))) continue;
            if (index == 5) PostQuitMessage(0);
            else Enter(destinations[index]);
            return;
        }
    }
    if (right && scene == Scene::Level2 && result == Result::Playing) {
        target = -1;
        float nearest = 200.f;
        for (size_t index = 0; index < enemies.size(); ++index) {
            if (!enemies[index].hp) continue;
            float distance;
            if (BoundingSphere(EnemyCenter(index), 2.2f).Intersects(nearPoint, direction, distance) && distance < nearest) {
                target = static_cast<int>(index);
                nearest = distance;
            }
        }
    }
}

void Game::Drag(int dx, int dy) {
    if (!IsLevel()) return;
    yaw = std::remainder(yaw + dx * .007f, XM_2PI);
    pitch = std::clamp(pitch - dy * .005f, -.58f, .52f);
}

void Game::Fire(const XMFLOAT3& origin, FXMVECTOR direction, bool friendly) {
    const XMVECTOR unit = XMVector3Normalize(direction);
    const float speed = friendly ? 43.f : 19.f;
    shots.push_back({origin, Store(unit * speed), 3.f, friendly});
    Burst(origin, .55f, false);
}

void Game::Burst(const XMFLOAT3& position, float size, bool major) {
    const int count = major ? 22 : 7;
    const float speed = major ? size * 1.25f : size * 1.8f;
    std::uniform_real_distribution<float> variation(-1.f, 1.f);
    for (int index = 0; index < count && fragments.size() < 240; ++index) {
        const XMFLOAT3 velocity{variation(random) * speed, (.6f + variation(random) * .5f) * speed,
            variation(random) * speed};
        const float life = (major ? .85f : .42f) + variation(random) * .12f;
        fragments.push_back({position, velocity, 0, life, .08f + size * .11f});
    }
}

void Game::UpdateShots(float dt) {
    for (auto& shot : shots) {
        const XMVECTOR previous = Vec(shot.position);
        const XMVECTOR velocity = Vec(shot.velocity);
        const XMVECTOR direction = XMVector3Normalize(velocity);
        const float length = XMVectorGetX(XMVector3Length(velocity)) * dt;
        shot.position = Store(previous + velocity * dt);
        shot.remaining -= dt;
        if (shot.remaining <= 0) continue;

        // Choose the nearest hit along this frame's segment.
        float nearest = length + .0001f;
        int hit = -1;
        auto test = [&](const XMFLOAT3& center, float radius, int id) {
            float distance;
            const BoundingSphere bounds(center, radius);
            if (bounds.Contains(previous) == CONTAINS) distance = 0;
            else if (!bounds.Intersects(previous, direction, distance)) return;
            if (distance < nearest) {
                nearest = distance;
                hit = id;
            }
        };
        auto& victims = scene == Scene::Level1 ? balloons : enemies;
        if (shot.friendly) {
            for (size_t index = 0; index < victims.size(); ++index) {
                if (!victims[index].hp) continue;
                const bool balloon = scene == Scene::Level1;
                test(balloon ? BalloonCenter(index) : EnemyCenter(index), balloon ? 1.35f : 2.05f,
                    static_cast<int>(index));
            }
        } else if (player.hp > 0) {
            test(PlayerCenter(), shield ? 4.f : 2.f, -2);
        }
        for (const auto& obstacle : obstacles) test(obstacle.position, obstacle.size * .62f, -3);

        if (hit == -1 && shot.position.y > GroundHeight(shot.position.x, shot.position.z)) continue;
        const XMFLOAT3 impact = hit == -1 ? shot.position : Store(previous + direction * nearest);
        shot.remaining = 0;
        if (hit >= 0) {
            Unit& victim = victims[hit];
            --victim.hp;
            if (target == hit && victim.hp == 0) target = -1;
            Burst(impact, victim.hp == 0 ? 6.f : 3.2f, victim.hp == 0);
        } else if (hit == -2) {
            if (!shield) player.hp = std::max(0, player.hp - 10);
            Burst(impact, player.hp == 0 ? 6.f : (shield ? 1.3f : 2.2f), player.hp == 0);
            if (player.hp == 0) result = Result::Lost;
        } else {
            Burst(impact, .85f, false);
        }
    }
    shots.erase(std::remove_if(shots.begin(), shots.end(),
        [](const Shot& shot) { return shot.remaining <= 0; }), shots.end());
}

int Game::Remaining() const {
    const auto& units = scene == Scene::Level1 ? balloons : enemies;
    return static_cast<int>(std::count_if(units.begin(), units.end(), [](const Unit& unit) { return unit.hp > 0; }));
}

void Game::Update(float dt) {
    time += dt;
    for (auto& fragment : fragments) {
        fragment.age += dt;
        fragment.position = Store(Vec(fragment.position) + Vec(fragment.velocity) * dt);
        fragment.velocity.y -= 10.f * dt;
    }
    fragments.erase(std::remove_if(fragments.begin(), fragments.end(),
        [](const Fragment& fragment) { return fragment.age >= fragment.life; }), fragments.end());
    if (scene == Scene::Intro && introExplosion >= 0 && time - introExplosion > 1.05f) Enter(Scene::Menu);
    if (!IsLevel()) return;

    if (result == Result::Playing) {
        const bool heli = IsHelicopter();
        const bool wasd = heli || Held(VK_SHIFT);
        const float ahead = float(Held(VK_UP) || (wasd && Held('W'))) - float(Held(VK_DOWN) || (wasd && Held('S')));
        const float side = float(Held(VK_RIGHT) || Held('D')) - float(Held(VK_LEFT) || (wasd && Held('A')));
        const XMVECTOR forward = XMVectorSet(std::sin(yaw), 0, std::cos(yaw), 0);
        const XMVECTOR right = XMVectorSet(std::cos(yaw), 0, -std::sin(yaw), 0);
        XMVECTOR movement = XMVector3Normalize(forward * ahead + right * side);
        XMFLOAT3 next = Store(Vec(player.position) + movement * (heli ? 16.f : 9.f) * dt);
        next.x = std::clamp(next.x, -76.f, 76.f);
        next.z = std::clamp(next.z, -76.f, 76.f);
        const float ground = GroundHeight(next.x, next.z);
        if (heli) {
            const float lift = float(Held('E')) - float(Held('Q'));
            next.y = std::clamp(next.y + lift * dt * 9.f, ground + 3.f, 45.f);
        } else {
            next.y = ground;
            const bool blocked = std::any_of(obstacles.begin(), obstacles.end(), [&](const Obstacle& obstacle) {
                return GroundDistance(next, obstacle.position) < obstacle.size * .65f + 1.65f;
            }) || std::any_of(enemies.begin(), enemies.end(), [&](const Unit& enemy) {
                return enemy.hp && GroundDistance(next, enemy.position) < 3.5f;
            });
            if (blocked) next = player.position;
        }
        player.position = next;
        UpdateCamera();

        const bool lock = !heli && automatic && target >= 0;
        player.turret = lock ? YawTo(player.position, enemies[target].position) : yaw;
        player.fireTimer -= dt;
        const bool firing = Held(VK_SPACE) || firePressed;
        firePressed = false;
        if ((firing || lock) && player.fireTimer <= 0) {
            const XMVECTOR gunForward = XMVectorSet(std::sin(player.turret), 0, std::cos(player.turret), 0);
            const XMFLOAT3 origin = heli ?
                Store(Vec(PlayerCenter()) + forward * 2.65f + XMVectorSet(0, firstPerson ? 1.1f : 0, 0, 0)) :
                Store(Vec(player.position) + gunForward * 5.9f + XMVectorSet(0, 2.15f, 0, 0));
            const XMVECTOR aim = lock ? Vec(EnemyCenter(target)) - Vec(origin) :
                gunForward + XMVectorSet(0, std::tan(pitch), 0, 0);
            Fire(origin, aim, true);
            player.fireTimer = heli ? .24f : .43f;
        }
        for (size_t index = 0; index < enemies.size(); ++index) {
            auto& enemy = enemies[index];
            if (!enemy.hp) continue;
            enemy.turret = YawTo(enemy.position, player.position);
            enemy.fireTimer -= dt;
            if (enemy.fireTimer > 0 || GroundDistance(enemy.position, player.position) > 32.f) continue;
            const XMFLOAT3 origin = Store(Vec(enemy.position) + XMVectorSet(
                std::sin(enemy.turret) * 5.9f, 2.15f, std::cos(enemy.turret) * 5.9f, 0));
            Fire(origin, Vec(PlayerCenter()) - Vec(origin), false);
            enemy.fireTimer = 3.4f + float(index % 3) * .24f;
        }
        UpdateShots(dt);
        if (scene == Scene::Level1 && GroundDistance(player.position, {0, 0, 65}) < 5.f) Enter(Scene::Level2);
        else if (scene != Scene::Level1 && result == Result::Playing && Remaining() == 0) result = Result::Won;
    } else {
        shots.clear();
    }
    UpdateCamera();
}


void Game::DrawTank(const Unit& unit, float hullYaw, bool friendly) {
    const auto origin = At(unit.position);
    const XMFLOAT4 color = friendly ? XMFLOAT4{.46f, .65f, .49f, 1} : XMFLOAT4{.8f, .43f, .33f, 1};
    const auto turret = XMMatrixRotationY(unit.turret) * XMMatrixTranslation(0, 1.66f, .1f) * origin;
    renderer.Draw(tank[0], XMMatrixRotationY(hullYaw) * origin, color);
    renderer.Draw(tank[1], turret, color);
    renderer.Draw(tank[2], XMMatrixTranslation(0, .49f, 1.1f) * turret, color);
}

void Game::DrawHelicopter() {
    const auto body = XMMatrixRotationY(yaw) * At(player.position);
    renderer.Draw(helicopter[0], body, Ice);
    renderer.Draw(helicopter[1], XMMatrixRotationY(time * 24.f) * XMMatrixTranslation(0, 3.65f, .3f) * body, Ice);
    renderer.Draw(helicopter[2], XMMatrixRotationX(time * 38.f) * XMMatrixTranslation(-.55f, 2.91f, -5.88f) * body, Ice);
}

void Game::DrawText3D(const TextMesh& text, const XMMATRIX& world, const XMFLOAT4& color) {
    renderer.Draw(text.mesh, world, color);
}

void Game::DrawFragments() {
    for (const auto& fragment : fragments) {
        const float size = fragment.size * (1.f - fragment.age / fragment.life);
        const auto world = XMMatrixScaling(size, size, size) *
            XMMatrixRotationRollPitchYaw(fragment.age * 6.f, fragment.age * 9.f, 0) * At(fragment.position);
        renderer.Draw(box, world, Amber);
    }
}

void Game::Render() {
    renderer.BeginFrame({.061f, .105f, .17f, 1});
    renderer.viewProjection = viewProjection;

    if (scene == Scene::Intro) {
        DrawText3D(title, XMMatrixRotationY(.35f * std::sin(time * 1.6f)) * XMMatrixTranslation(0, 2.55f, 0), Ice);
        if (introExplosion < 0) {
            DrawText3D(name, IntroName(), Amber);
            DrawText3D(introHint, XMMatrixScaling(.56f, .56f, .56f) * XMMatrixTranslation(0, -3.35f, 0), Mint);
        }
    } else if (scene == Scene::Menu) {
        DrawText3D(menuTitle, XMMatrixTranslation(0, 5.45f, 0), Amber);
        for (size_t index = 0; index < menu.size(); ++index)
            DrawText3D(menu[index], MenuItem(index), index == 4 ? Mint : Ice);
    } else if (scene == Scene::Tutorial) {
        for (size_t index = 0; index < tutorial.size(); ++index)
            DrawText3D(tutorial[index], XMMatrixScaling(.62f, .62f, .62f) *
                XMMatrixTranslation(0, 4.1f - 1.6f * float(index), 0), index == 0 ? Mint : Ice);
    } else {
        renderer.Draw(terrain, XMMatrixIdentity(), {.36f, .49f, .3f, 1});
        for (const auto& obstacle : obstacles) {
            const auto world = XMMatrixScaling(obstacle.size, obstacle.size, obstacle.size) *
                XMMatrixRotationY(obstacle.yaw) * At(obstacle.position);
            renderer.Draw(box, world, {.39f, .36f, .30f, 1});
        }
        for (size_t index = 0; index < enemies.size(); ++index) {
            if (!enemies[index].hp) continue;
            DrawTank(enemies[index], XM_PI, false);
            if (target == static_cast<int>(index)) {
                const auto p = enemies[index].position;
                renderer.Draw(box, XMMatrixScaling(.75f, .75f, .75f) *
                    XMMatrixRotationRollPitchYaw(.75f, time * 2.5f, .78f) *
                    XMMatrixTranslation(p.x, p.y + 5.4f, p.z), Amber);
            }
        }
        for (size_t index = 0; index < balloons.size(); ++index) {
            if (!balloons[index].hp) continue;
            renderer.Draw(sphere, XMMatrixScaling(1.15f, 1.42f, 1.15f) * At(BalloonCenter(index)),
                index % 2 ? Red : Amber);
        }
        if (player.hp && !firstPerson) {
            if (IsHelicopter()) DrawHelicopter();
            else DrawTank(player, yaw, true);
        }
        for (const auto& shot : shots) {
            renderer.Draw(sphere, XMMatrixScaling(.14f, .14f, .14f) * At(shot.position),
                shot.friendly ? Amber : Red);
        }
        if (scene == Scene::Level1) {
            const float ground = GroundHeight(0, 65);
            renderer.Draw(box, XMMatrixScaling(10, .4f, 10) * XMMatrixTranslation(0, ground, 65), Mint);
            DrawText3D(goalText, XMMatrixRotationY(time * .45f) * XMMatrixTranslation(0, ground + 9, 65), Mint);
        }
        if (result != Result::Playing) {
            const auto textWorld = XMMatrixTranslation(0, .45f, 11) * XMMatrixInverse(nullptr, view);
            DrawText3D(result == Result::Won ? winText : lostText, textWorld, result == Result::Won ? Mint : Red);
        }
    }

    DrawFragments();
    if (scene == Scene::Level1) {
        const float ground = GroundHeight(0, 65);
        renderer.Draw(box, XMMatrixScaling(4, 12, 4) * XMMatrixTranslation(0, ground + 6, 65),
            {.15f, 1.f, .63f, .16f}, Pass::Alpha);
    }
    if (shield && player.hp) {
        renderer.Draw(sphere, XMMatrixScaling(4, 3.05f, 4) * At(PlayerCenter()),
            {.18f, .85f, 1.f, .2f}, Pass::Alpha);
    }
    renderer.EndFrame();
}

namespace {
Game* activeGame = nullptr;
POINT previousMouse{};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM detail) {
    switch (message) {
    case WM_KEYDOWN:
        if (activeGame && !(detail & (1LL << 30))) activeGame->Key(key);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        previousMouse = {static_cast<short>(LOWORD(detail)), static_cast<short>(HIWORD(detail))};
        if (activeGame) activeGame->Click(previousMouse.x, previousMouse.y, message == WM_RBUTTONDOWN);
        if (message == WM_LBUTTONDOWN) SetCapture(window);
        return 0;
    case WM_MOUSEMOVE:
        if (activeGame && (key & MK_LBUTTON) && GetCapture() == window) {
            const POINT position{static_cast<short>(LOWORD(detail)), static_cast<short>(HIWORD(detail))};
            activeGame->Drag(position.x - previousMouse.x, position.y - previousMouse.y);
            previousMouse = position;
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == window) ReleaseCapture();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, key, detail);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int status = 0;
    try {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, _countof(executable));
        auto folder = std::filesystem::path(executable).parent_path();
        for (int level = 0; level < 3 && !folder.empty(); ++level) {
            if (std::filesystem::is_directory(folder / "Assets") &&
                std::filesystem::is_regular_file(folder / "Shaders.hlsl")) {
                std::filesystem::current_path(folder);
                break;
            }
            folder = folder.parent_path();
        }
        SetProcessDPIAware();
        WNDCLASSW windowClass{};
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.lpszClassName = L"3DGP2_Project_01_Window";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&windowClass)) throw std::runtime_error("Window Class를 등록하지 못했음");
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT bounds{0, 0, Width, Height};
        AdjustWindowRect(&bounds, style, FALSE);
        HWND window = CreateWindowW(windowClass.lpszClassName, L"3D Game Programming 1 - Assignment 4",
            style, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("게임 창을 만들지 못했음");

        Game game;
        game.Init(window);
        activeGame = &game;
        ShowWindow(window, show);
        using Clock = std::chrono::steady_clock;
        auto lastFrame = Clock::now();
        MSG message{};
        for (;;) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) break;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (message.message == WM_QUIT) break;
            if (IsIconic(window)) {
                WaitMessage();
                lastFrame = Clock::now();
                continue;
            }
            const auto now = Clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - lastFrame).count(), .0001f, .05f);
            lastFrame = now;
            game.Update(dt);
            game.Render();
        }
        activeGame = nullptr;
        if (IsWindow(window)) DestroyWindow(window);
    } catch (const std::exception& error) {
        activeGame = nullptr;
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        int length = MultiByteToWideChar(codePage, flags, error.what(), -1, nullptr, 0);
        if (length == 0) {
            codePage = CP_ACP;
            flags = 0;
            length = MultiByteToWideChar(codePage, flags, error.what(), -1, nullptr, 0);
        }
        std::wstring message = L"게임 실행 중 오류가 발생했음";
        if (length > 1) {
            message.resize(length);
            if (MultiByteToWideChar(codePage, flags, error.what(), -1, message.data(), length) == 0)
                message = L"오류 메시지를 읽지 못했음";
        }
        MessageBoxW(nullptr, message.c_str(), L"3DGP2 오류", MB_OK | MB_ICONERROR);
        status = 1;
    }
    return status;
}
