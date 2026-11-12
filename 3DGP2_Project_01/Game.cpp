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
constexpr XMFLOAT3 IntroNamePosition{0, -.45f, 0};

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
    float remaining;
    bool friendly;
};
struct Impact {
    XMFLOAT3 position;
    float start, duration, size;
};
struct Obstacle {
    XMFLOAT3 position;
    float size;
};
}

class Game {
public:
    void Init(HWND window);
    void Resize(UINT width, UINT height);
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
    void SpawnImpact(const XMFLOAT3& position, float size, bool large);
    void DrawTank(const Unit& unit, float hullYaw, bool friendly);
    void DrawHelicopter();
    void DrawImpacts();
    void DrawHealthLabels();
    void DrawHealthNumber(int hp, const XMMATRIX& world, const XMFLOAT4& color);
    XMMATRIX BillboardRotation() const;
    XMMATRIX IntroName() const;
    XMMATRIX MenuItem(size_t index) const;
    XMFLOAT3 EnemyCenter(size_t index) const;
    XMFLOAT3 BalloonCenter(size_t index) const;
    XMFLOAT3 PlayerCenter() const;
    bool IsHelicopter() const { return scene == Scene::Level1 || scene == Scene::Level3; }
    bool IsLevel() const { return scene >= Scene::Level1; }

    Renderer renderer;
    Mesh box, sphere, terrain, impactPoint, treePoints;
    std::array<Mesh, 3> tank, helicopter;
    std::array<Mesh, 6> skyFaces;
    Mesh title, introHint, menuTitle, winText, lostText, goalText, tutorial;
    TextMesh name{};
    std::array<TextMesh, 6> menu;
    std::array<Mesh, 10> healthDigits;
    Scene scene = Scene::Intro;
    Result result = Result::Playing;
    Unit player;
    std::vector<Unit> enemies, balloons;
    std::vector<Shot> shots;
    std::vector<Impact> impacts;
    std::vector<Obstacle> obstacles;
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX viewProjection = XMMatrixIdentity();
    float yaw = 0, pitch = 0, time = 0;
    int target = -1;
    bool introExploding = false;
    bool automatic = false, shield = false, firstPerson = false, firePressed = false;
};

void Game::Init(HWND window) {
    renderer.Init(window);
    box = MakeBox(renderer);
    skyFaces = MakeSkyboxFaces(renderer);
    sphere = MakeSphere(renderer);
    terrain = MakeTerrain(renderer);
    impactPoint = renderer.MakeMesh({Vertex{{0, 0, 0}, {0, 0, 0}}});
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
    // IntroText.dds에서 제목, 이름, 안내 문구에 각각 다른 UV 영역을 사용한다.
    title = MakeTexturedQuad(renderer, 14.0f, 3.5f, 0.5f, 0.125f, 1.0f, 0.375f).mesh;
    name = MakeTexturedQuad(renderer, 4.6f, 2.3f, 0.0f, 0.0f, 0.5f, 0.5f);
    introHint = MakeTexturedQuad(renderer, 10.4f, 2.6f, 0.0f, 0.5f, 1.0f, 1.0f).mesh;
    menuTitle = MakeTexturedQuad(renderer, 9.6f, 2.4f, 0.0f, 0.0f, 1.0f, 0.125f).mesh;
    // GameText.dds의 세 줄을 각각 하나의 사각형에 매핑한다.
    constexpr float gameTextWidth = 14.0f;
    constexpr float gameTextHeight = gameTextWidth / 6.0f;
    constexpr float textRow = 1.0f / 3.0f;
    winText = MakeTexturedQuad(renderer, gameTextWidth, gameTextHeight, 0, 0, 1, textRow).mesh;
    lostText = MakeTexturedQuad(renderer, gameTextWidth, gameTextHeight, 0, textRow, 1, 2 * textRow).mesh;
    goalText = MakeTexturedQuad(renderer, gameTextWidth, gameTextHeight, 0, 2 * textRow, 1, 1).mesh;

    // 투명 여백을 잘라 클릭 범위를 각 메뉴 항목의 글자 크기에 맞춘다.
    constexpr float menuHalfWidths[] = {168, 148, 148, 148, 108, 78};
    constexpr float menuUnitsPerPixel = 10.5f / 512.0f;
    for (size_t index = 0; index < menu.size(); ++index) {
        const float rowTop = static_cast<float>((index + 1) * 128);
        const float halfWidth = menuHalfWidths[index];
        const float uOffset = halfWidth / 512.0f;
        menu[index] = MakeTexturedQuad(renderer,
            2.0f * halfWidth * menuUnitsPerPixel, 76.0f * menuUnitsPerPixel,
            0.5f - uOffset, (rowTop + 26.0f) / 1024.0f,
            0.5f + uOffset, (rowTop + 102.0f) / 1024.0f);
    }
    tutorial = MakeTexturedQuad(renderer, 30.72f, 9.6f, 0.0f, 0.0f, 1.0f, 1.0f).mesh;

    // 숫자마다 4×4 아틀라스에서 사용할 UV 영역을 지정한다.
    constexpr float digitCell = .25f;
    for (size_t digit = 0; digit < healthDigits.size(); ++digit) {
        const float u = static_cast<float>(digit % 4) * digitCell;
        const float v = static_cast<float>(digit / 4) * digitCell;
        healthDigits[digit] = MakeTexturedQuad(renderer, 1.6f, 1.6f,
            u, v, u + digitCell, v + digitCell).mesh;
    }

    Enter(Scene::Intro);
}

void Game::Resize(UINT width, UINT height) {
    if (renderer.Resize(width, height)) UpdateCamera();
}

void Game::Enter(Scene next) {
    scene = next;
    result = Result::Playing;
    time = 0;
    introExploding = false;
    shots.clear();
    impacts.clear();
    enemies.clear();
    balloons.clear();
    obstacles.clear();
    treePoints = {};
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

        // 이동 경로와 탱크 시작 위치에는 장애물을 배치하지 않는다.
        std::mt19937 placement(4);
        std::uniform_real_distribution<float> position(-68.f, 68.f), size(1.1f, 2.8f);
        const size_t count = scene == Scene::Level2 ? 55 : 26;
        std::vector<Vertex> trees;
        trees.reserve(count);
        while (obstacles.size() < count) {
            const float x = position(placement), z = position(placement);
            if (std::abs(x) < 24 && z > -20 && z < 53) continue;
            const float scale = size(placement);
            const float ground = GroundHeight(x, z);
            obstacles.push_back({{x, ground + scale * .5f, z}, scale});

            const float height = scale * 2.4f;
            const float width = height * (2.0f / 3.0f);
            // DDS 아래쪽 투명 여백을 지면 아래로 내려 밑동을 맞춘다.
            constexpr float bottomPadding = 9.0f / 768.0f;
            const float centerY = ground + height * (.5f - bottomPadding);
            trees.push_back({{x, centerY, z}, {0, 0, 0}, {width, height}});
        }
        treePoints = renderer.MakeMesh(trees);
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
    XMStoreFloat3(&renderer.cameraPosition, from);
    view = XMMatrixLookAtLH(from, to, XMVectorSet(0, 1, 0, 0));
    const float aspect = static_cast<float>(renderer.Width()) / renderer.Height();
    float fov = XMConvertToRadians(57);
    // 좁은 창에서도 인트로와 튜토리얼의 좌우 끝이 잘리지 않도록 한다.
    constexpr float uiAspect = 16.0f / 9.0f;
    if (!IsLevel() && aspect < uiAspect) {
        fov = 2.0f * std::atan(std::tan(fov * .5f) * uiAspect / aspect);
    }
    viewProjection = view * XMMatrixPerspectiveFovLH(fov, aspect, .15f, 380.f);
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
    return XMMatrixRotationY(.65f * std::sin(time * 2.4f)) * At(IntroNamePosition);
}

XMMATRIX Game::MenuItem(size_t index) const {
    return XMMatrixScaling(.72f, .72f, .72f) * XMMatrixTranslation(0, 3.05f - 1.46f * float(index), 0);
}

void Game::Click(int x, int y, bool right) {
    const XMMATRIX inverse = XMMatrixInverse(nullptr, viewProjection);
    const float ndcX = 2.f * x / renderer.Width() - 1.f;
    const float ndcY = 1.f - 2.f * y / renderer.Height();
    const auto nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0, 1), inverse);
    const auto farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1, 1), inverse);
    const auto direction = XMVector3Normalize(farPoint - nearPoint);
    auto textHit = [&](const TextMesh& text, const XMMATRIX& world) {
        const auto local = XMMatrixInverse(nullptr, world);
        const BoundingBox bounds({0,0,0}, {text.width / 2 + .25f, text.height / 2 + .18f, .2f});
        float distance = 0;
        return bounds.Intersects(XMVector3TransformCoord(nearPoint, local),
            XMVector3Normalize(XMVector3TransformNormal(direction, local)), distance);
    };

    if (!right && scene == Scene::Intro && !introExploding && textHit(name, IntroName())) {
        SpawnImpact(IntroNamePosition, 6.f, true);
        introExploding = true;
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
}

void Game::SpawnImpact(const XMFLOAT3& position, float size, bool large) {
    impacts.push_back({position, time, large ? .75f : .38f, size});
}

void Game::UpdateShots(float dt) {
    const bool balloonLevel = scene == Scene::Level1;
    auto& victims = balloonLevel ? balloons : enemies;
    for (auto& shot : shots) {
        const XMVECTOR previous = Vec(shot.position);
        const XMVECTOR velocity = Vec(shot.velocity);
        const XMVECTOR direction = XMVector3Normalize(velocity);
        const float length = XMVectorGetX(XMVector3Length(velocity)) * dt;
        shot.position = Store(previous + velocity * dt);
        shot.remaining -= dt;
        if (shot.remaining <= 0) continue;

        // 이번 프레임에 탄환이 이동한 구간에서 가장 가까운 충돌을 찾는다.
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
        if (shot.friendly) {
            for (size_t index = 0; index < victims.size(); ++index) {
                if (!victims[index].hp) continue;
                test(balloonLevel ? BalloonCenter(index) : EnemyCenter(index), balloonLevel ? 1.35f : 2.05f,
                    static_cast<int>(index));
            }
        } else if (player.hp > 0) {
            test(PlayerCenter(), shield ? 4.f : 2.f, -2);
        }
        for (const auto& obstacle : obstacles) test(obstacle.position, obstacle.size * .62f, -3);

        XMFLOAT3 impact;
        if (hit == -1) {
            const float ground = GroundHeight(shot.position.x, shot.position.z);
            if (shot.position.y > ground) continue;
            impact = {shot.position.x, ground + .2f, shot.position.z};
        } else {
            impact = Store(previous + direction * (nearest - .06f));
        }
        shot.remaining = 0;
        if (hit >= 0) {
            Unit& victim = victims[hit];
            --victim.hp;
            if (target == hit && victim.hp == 0) target = -1;
            SpawnImpact(impact, victim.hp == 0 ? 6.f : 3.2f, victim.hp == 0);
        } else if (hit == -2) {
            if (!shield) player.hp = std::max(0, player.hp - 10);
            SpawnImpact(impact, player.hp == 0 ? 6.f : (shield ? 1.8f : 2.8f), player.hp == 0);
            if (player.hp == 0) result = Result::Lost;
        } else {
            SpawnImpact(impact, 1.6f, false);
        }
    }
    shots.erase(std::remove_if(shots.begin(), shots.end(),
        [](const Shot& shot) { return shot.remaining <= 0; }), shots.end());
}

void Game::Update(float dt) {
    time += dt;
    impacts.erase(std::remove_if(impacts.begin(), impacts.end(),
        [this](const Impact& impact) { return time - impact.start >= impact.duration; }), impacts.end());
    if (scene == Scene::Intro && introExploding && impacts.empty()) Enter(Scene::Menu);
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
        if (scene == Scene::Level1 && GroundDistance(player.position, {0, 0, 65}) < 5.f) {
            Enter(Scene::Level2);
            return;
        }
        if (scene != Scene::Level1 && result == Result::Playing &&
            std::none_of(enemies.begin(), enemies.end(), [](const Unit& enemy) { return enemy.hp > 0; })) {
            result = Result::Won;
        }
    } else {
        shots.clear();
    }
    UpdateCamera();
}


void Game::DrawTank(const Unit& unit, float hullYaw, bool friendly) {
    const auto origin = At(unit.position);
    const XMFLOAT4 color = friendly ? XMFLOAT4{1, 1, 1, 1} : XMFLOAT4{1, .72f, .65f, 1};
    const auto turret = XMMatrixRotationY(unit.turret) * XMMatrixTranslation(0, 1.66f, .1f) * origin;
    renderer.BindTexture(TextureId::Tank);
    renderer.Draw(tank[0], XMMatrixRotationY(hullYaw) * origin, color, Pass::Model);
    renderer.Draw(tank[1], turret, color, Pass::Model);
    renderer.Draw(tank[2], XMMatrixTranslation(0, .49f, 1.1f) * turret, color, Pass::Model);
}

void Game::DrawHelicopter() {
    const auto body = XMMatrixRotationY(yaw) * At(player.position);
    const XMFLOAT4 color{1, 1, 1, 1};
    const auto mainRotor = XMMatrixRotationY(time * 24.f) * XMMatrixTranslation(0, 3.65f, .3f) * body;
    const auto tailRotor = XMMatrixRotationX(time * 38.f) * XMMatrixTranslation(-.55f, 2.91f, -5.88f) * body;
    renderer.BindTexture(TextureId::Helicopter);
    renderer.Draw(helicopter[0], body, color, Pass::Model);
    renderer.Draw(helicopter[1], mainRotor, color, Pass::Model);
    renderer.Draw(helicopter[2], tailRotor, color, Pass::Model);
}

XMMATRIX Game::BillboardRotation() const {
    // 카메라의 회전만 적용해 사각형이 화면을 향하도록 한다.
    auto cameraRotation = XMMatrixInverse(nullptr, view);
    cameraRotation.r[3] = XMVectorSet(0, 0, 0, 1);
    return cameraRotation;
}

void Game::DrawImpacts() {
    if (impacts.empty()) return;
    renderer.BindTexture(TextureId::Impact);
    const auto cameraRotation = BillboardRotation();
    for (const auto& impact : impacts) {
        const auto world = XMMatrixScaling(impact.size, impact.size, impact.size) *
            cameraRotation * At(impact.position);
        const float progress = (time - impact.start) / impact.duration;
        renderer.Draw(impactPoint, world, {1, 1, 1, 1}, Pass::Impact, progress);
    }
}

void Game::DrawHealthNumber(int hp, const XMMATRIX& world, const XMFLOAT4& color) {
    if (hp <= 0) return;
    const auto digits = std::to_string(hp);
    constexpr float spacing = .85f;
    float x = -.5f * static_cast<float>(digits.size() - 1) * spacing;
    for (const char digit : digits) {
        renderer.Draw(healthDigits[digit - '0'], XMMatrixTranslation(x, 0, 0) * world, color, Pass::Font);
        x += spacing;
    }
}

void Game::DrawHealthLabels() {
    renderer.BindTexture(TextureId::Digits);
    const auto facing = BillboardRotation();

    for (const auto& enemy : enemies) {
        const auto& p = enemy.position;
        DrawHealthNumber(enemy.hp, facing * XMMatrixTranslation(p.x, p.y + 4.1f, p.z), Red);
    }
    for (size_t index = 0; index < balloons.size(); ++index) {
        auto p = BalloonCenter(index);
        p.y += 2.15f;
        DrawHealthNumber(balloons[index].hp, facing * At(p), Ice);
    }
    if (!firstPerson) {
        const auto& p = player.position;
        const float height = IsHelicopter() ? 5.2f : 4.1f;
        DrawHealthNumber(player.hp, facing * XMMatrixTranslation(p.x, p.y + height, p.z), Mint);
    }
}

void Game::Render() {
    renderer.BeginFrame({.061f, .105f, .17f, 1});
    renderer.viewProjection = viewProjection;

    if (scene == Scene::Intro) {
        renderer.BindTexture(TextureId::Intro);
        const auto titleWorld = XMMatrixRotationY(.35f * std::sin(time * 1.6f)) *
            XMMatrixTranslation(0, 2.55f, 0);
        renderer.Draw(title, titleWorld, Ice, Pass::Font);
        if (!introExploding) {
            renderer.Draw(name.mesh, IntroName(), Amber, Pass::Font);
            renderer.Draw(introHint, XMMatrixTranslation(0, -3.35f, 0), Mint, Pass::Font);
        }
    } else if (scene == Scene::Menu) {
        renderer.BindTexture(TextureId::Menu);
        renderer.Draw(menuTitle, XMMatrixTranslation(0, 5.45f, 0), Amber, Pass::Font);
        for (size_t index = 0; index < menu.size(); ++index)
            renderer.Draw(menu[index].mesh, MenuItem(index), index == 4 ? Mint : Ice, Pass::Font);
    } else if (scene == Scene::Tutorial) {
        renderer.BindTexture(TextureId::Tutorial);
        renderer.Draw(tutorial, XMMatrixTranslation(0, 0.1f, 0), {1, 1, 1, 1}, Pass::Font);
    } else {
        renderer.Draw(terrain, XMMatrixIdentity(), {.36f, .49f, .3f, 1});
        renderer.BindTexture(TextureId::Tree);
        renderer.Draw(treePoints, XMMatrixIdentity(), {1, 1, 1, 1}, Pass::Tree);
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

        constexpr std::array<TextureId, 6> skyTextures{
            TextureId::SkyRight, TextureId::SkyLeft, TextureId::SkyTop,
            TextureId::SkyBottom, TextureId::SkyFront, TextureId::SkyBack
        };
        const auto world = At(renderer.cameraPosition);
        for (size_t face = 0; face < skyFaces.size(); ++face) {
            renderer.BindTexture(skyTextures[face]);
            renderer.Draw(skyFaces[face], world, {1, 1, 1, 1}, Pass::Skybox);
        }
        if (scene == Scene::Level1) {
            const float ground = GroundHeight(0, 65);
            renderer.Draw(box, XMMatrixScaling(10, .4f, 10) * XMMatrixTranslation(0, ground, 65), Mint);
            renderer.Draw(box, XMMatrixScaling(4, 12, 4) * XMMatrixTranslation(0, ground + 6, 65),
                {.15f, 1.f, .63f, .16f}, Pass::Alpha);
            renderer.BindTexture(TextureId::GameText);
            const auto goalWorld = XMMatrixRotationY(time * .45f) * XMMatrixTranslation(0, ground + 9, 65);
            renderer.Draw(goalText, goalWorld, Mint, Pass::Font);
        }
        if (shield && player.hp) {
            renderer.Draw(sphere, XMMatrixScaling(4, 3.05f, 4) * At(PlayerCenter()),
                {.18f, .85f, 1.f, .2f}, Pass::Alpha);
        }
    }
    DrawImpacts();
    if (IsLevel()) DrawHealthLabels();
    if (result != Result::Playing) {
        // 결과 문구는 화면 위쪽에 배치하고 전투 화면 위에 그린다.
        const float aspect = static_cast<float>(renderer.Width()) / renderer.Height();
        const float overlayHeight = std::max(12.0f, 20.0f / aspect);
        renderer.viewProjection = XMMatrixOrthographicLH(overlayHeight * aspect, overlayHeight, 0, 1);
        renderer.BindTexture(TextureId::GameText);
        const bool won = result == Result::Won;
        renderer.Draw(won ? winText : lostText, XMMatrixTranslation(0, overlayHeight * .14f, 0),
            won ? Mint : Red, Pass::Font);
    }
    renderer.EndFrame();
}

namespace {
constexpr DWORD WindowedStyle = WS_OVERLAPPEDWINDOW;
constexpr LONG DefaultClientWidth = 1600;
constexpr LONG DefaultClientHeight = 900;
Game* activeGame = nullptr;
POINT previousMouse{};
SIZE pendingClientSize{};
WINDOWPLACEMENT windowedPlacement{sizeof(WINDOWPLACEMENT)};
bool fullscreen = false;

RECT InitialWindowBounds() {
    RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    RECT frame{};
    AdjustWindowRectExForDpi(&frame, WindowedStyle, FALSE, 0, GetDpiForSystem());
    const LONG frameWidth = frame.right - frame.left;
    const LONG frameHeight = frame.bottom - frame.top;
    const LONG workWidth = workArea.right - workArea.left;
    const LONG workHeight = workArea.bottom - workArea.top;
    const double scale = std::min({
        1.0, static_cast<double>(workWidth - frameWidth) / DefaultClientWidth,
        static_cast<double>(workHeight - frameHeight) / DefaultClientHeight
    });
    const LONG width = std::max(1L, static_cast<LONG>(DefaultClientWidth * scale)) + frameWidth;
    const LONG height = std::max(1L, static_cast<LONG>(DefaultClientHeight * scale)) + frameHeight;
    const LONG left = workArea.left + (workWidth - width) / 2;
    const LONG top = workArea.top + (workHeight - height) / 2;
    return {left, top, left + width, top + height};
}

bool FitFullscreen(HWND window) {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const auto& bounds = monitor.rcMonitor;
    return SetWindowPos(window, HWND_TOP, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER) != FALSE;
}

void ToggleFullscreen(HWND window) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!fullscreen) {
        if (!GetWindowPlacement(window, &windowedPlacement)) return;
        SetWindowLongPtrW(window, GWL_STYLE,
            (style & ~static_cast<LONG_PTR>(WindowedStyle | WS_MAXIMIZE)) | WS_POPUP);
        fullscreen = true;
        if (FitFullscreen(window)) return;
    }

    fullscreen = false;
    SetWindowLongPtrW(window, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WindowedStyle);
    SetWindowPlacement(window, &windowedPlacement);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM detail) {
    switch (message) {
    case WM_SIZE:
        if (key != SIZE_MINIMIZED) pendingClientSize = {LOWORD(detail), HIWORD(detail)};
        return 0;
    case WM_DPICHANGED:
        if (fullscreen) {
            FitFullscreen(window);
        } else {
            const auto& bounds = *reinterpret_cast<const RECT*>(detail);
            SetWindowPos(window, nullptr, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (fullscreen) FitFullscreen(window);
        break;
    case WM_SYSKEYDOWN:
        if (key == VK_RETURN && (detail & (1LL << 29))) {
            if (activeGame && !(detail & (1LL << 30))) ToggleFullscreen(window);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (key == VK_RETURN) return 0;
        break;
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
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
        WNDCLASSW windowClass{};
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.lpszClassName = L"3DGP2_Project_01_Window";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&windowClass)) throw std::runtime_error("Window Class를 등록하지 못했음");
        const RECT bounds = InitialWindowBounds();
        HWND window = CreateWindowW(windowClass.lpszClassName, L"3D Game Programming 1 - Assignment 4",
            WindowedStyle, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
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
            game.Resize(static_cast<UINT>(pendingClientSize.cx), static_cast<UINT>(pendingClientSize.cy));
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
