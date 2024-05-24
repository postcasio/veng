import { readFileSync, writeFileSync } from 'fs';

console.log(process.argv);

const mapFile = readFileSync(process.argv[2], 'utf8');

/*
entity looks like:

// entity 1
{
"classname" "light_point"
"origin" "-7 1033 513"
"constant" "1"
"intensity" "1"
"color" "0.740353 0.249699 0.501213"
}

match the entire block with regex, then use regex to add colons and commas to make it valid JSON
*/

const entityRegex = /^(\/\/ entity \d+\n)\{[\s\S]*?\}/gm;

const entities = mapFile.matchAll(entityRegex);
const jsonEntities = [];

if (entities) {
    for (const entityMatch of entities) {
        if (entityMatch[0].indexOf('"classname" "worldspawn"') !== -1) {
            continue;
        }

        const entity =
            JSON.parse(entityMatch[0]
                .replace(/"(\w+)"\s+"([^"]*)"/g, '"$1": "$2",')
                .replace(/"(\w+)"\s+"([^"]*)"\n/g, '"$1": "$2"\n')
                .substring(entityMatch[1].length)
                .replace(/,\n\}/, '\n}'))
        
        entity.origin = entity.origin.split(' ').map(Number);
        
        entity.origin = [-entity.origin[0], entity.origin[2], -entity.origin[1]];

        if (entity.color) {
            entity.color = entity.color.split(' ').map(Number);
        }

        if (entity.constant) {
            entity.constant = Number(entity.constant);
        }

        if (entity.intensity) {
            entity.intensity = Number(entity.intensity);
        }

        if (entity.quadratic) {
            entity.quadratic = Number(entity.quadratic);
        }

        if (entity.linear) {
            entity.linear = Number(entity.linear);
        }

        jsonEntities.push(entity);
    }
}

writeFileSync(process.argv[2].replace(/\.map$/, '.json'), JSON.stringify(jsonEntities, null, 2));
