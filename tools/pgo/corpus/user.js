function makeUser(id) {
    return {
        id,
        name: "user_" + id,
        score: id % 100,
        active: (id & 1) === 0,
        tags: ["js", "vm", "bench", id],
        get rank() {
            return this.score * 2;
        },
        inc(x) {
            this.score += x;
        }
    };
}

const users = [];

for (let i = 0; i < 100000; i++) {
    users.push(makeUser(i));
}

let checksum = 0;

for (let r = 0; r < 100; r++) {
    for (let i = 0; i < users.length; i++) {
        const u = users[i];

        u.inc(1);

        if (u.active) {
            checksum += u.rank;
        } else {
            checksum += u.score;
        }

        checksum += u.tags.length;
    }
}

console.log("checksum=" + checksum);